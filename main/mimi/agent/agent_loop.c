#include "agent_loop.h"
#include "agent/context_builder.h"
#include "agent/runner.h"
#include "agent/learning_hooks.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "memory/session_manager.h"
#include "memory/consolidator.h"
#include "tools/tool_registry.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

static const char *TAG = "agent";

/* Forward declaration */
static void agent_loop_task(void *arg);
static void *s_agent_stack = NULL;
static StaticTask_t s_agent_tcb;
static TaskHandle_t s_agent_task_handle = NULL;
static bool s_agent_psram_stack = false;

static void agent_log_runtime_resources(const char *stage)
{
    ESP_LOGI(TAG, "%s: stack_free=%u bytes internal_free=%u internal_largest=%u psram_free=%u",
             stage,
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void *agent_alloc_stack(uint32_t stack_words, const char **heap_name)
{
    size_t stack_bytes = (size_t)stack_words * sizeof(StackType_t);

    /* Try internal DRAM first — safe for flash/FATFS write ops */
    void *stack = heap_caps_malloc(stack_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (stack) {
        *heap_name = "internal";
        s_agent_psram_stack = false;
        return stack;
    }

    /* Internal heap too fragmented — fall back to PSRAM.
       Flash writes (session save, consolidator, memory update) will be
       skipped to avoid esp_task_stack_is_sane_cache_disabled() assert. */
    stack = heap_caps_malloc(stack_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (stack) {
        *heap_name = "psram";
        s_agent_psram_stack = true;
        return stack;
    }

    *heap_name = "none";
    s_agent_psram_stack = false;
    return NULL;
}

esp_err_t agent_loop_init(void)
{
    /* Initialize runner subsystem */
    agent_runner_init();

    /* Initialize session consolidator */
    consolidator_init(NULL);  /* Use default config */

    ESP_LOGI(TAG, "Agent loop initialized");
    return ESP_OK;
}

esp_err_t agent_loop_start(void)
{
    if (s_agent_task_handle) {
        return ESP_OK;
    }

    const uint32_t stack_candidates[] = {
        MIMI_AGENT_STACK,
        20 * 1024,
        16 * 1024,
        14 * 1024,
        12 * 1024,
        10 * 1024,
         8 * 1024,
         6 * 1024,
         5 * 1024,
         4 * 1024,
    };

    for (size_t i = 0; i < (sizeof(stack_candidates) / sizeof(stack_candidates[0])); i++) {
        uint32_t stack_words = stack_candidates[i];
        const char *heap_name = NULL;
        s_agent_stack = agent_alloc_stack(stack_words, &heap_name);
        if (!s_agent_stack) {
            ESP_LOGW(TAG, "agent_loop stack alloc failed (stack=%u, psram_free=%u, internal_free=%u), retrying...",
                     (unsigned)stack_words,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            continue;
        }

        size_t stack_bytes = (size_t)stack_words * sizeof(StackType_t);

        TaskHandle_t h = xTaskCreateStaticPinnedToCore(
            agent_loop_task, "agent_loop",
            stack_words, NULL,
            MIMI_AGENT_PRIO,
            (StackType_t *)s_agent_stack,
            &s_agent_tcb,
            MIMI_AGENT_CORE);

        if (h) {
            s_agent_task_handle = h;
            ESP_LOGI(TAG, "agent_loop task created with stack=%u words (%u bytes, %s, internal_largest=%u)",
                     (unsigned)stack_words, (unsigned)stack_bytes, heap_name,
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            return ESP_OK;
        }

        ESP_LOGW(TAG, "agent_loop create failed (stack=%u words, heap=%s), retrying...",
                 (unsigned)stack_words, heap_name);
        heap_caps_free(s_agent_stack);
        s_agent_stack = NULL;
    }

    return ESP_FAIL;
}

/* ─── Main Agent Loop Task ────────────────────────────────────────────── */

static void agent_loop_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Agent loop started on core %d", xPortGetCoreID());
    agent_log_runtime_resources("agent_loop start");

    /* Allocate buffers from PSRAM */
    char *system_prompt = heap_caps_calloc(1, MIMI_CONTEXT_BUF_SIZE, MALLOC_CAP_SPIRAM);
    char *history_json = heap_caps_calloc(1, MIMI_LLM_STREAM_BUF_SIZE, MALLOC_CAP_SPIRAM);

    if (!system_prompt || !history_json) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM buffers");
        vTaskDelete(NULL);
        return;
    }

    const char *tools_json = tool_registry_get_tools_json();

    while (1) {
        mimi_msg_t msg;
        esp_err_t err = message_bus_pop_inbound(&msg, portMAX_DELAY);
        if (err != ESP_OK) continue;

        ESP_LOGI(TAG, "Processing message from %s:%s", msg.channel, msg.chat_id);

        /* Skip LLM processing for non-xiaozhi channels */
        if (strcmp(msg.channel, MIMI_CHAN_XIAOZHI) != 0) {
            ESP_LOGI(TAG, "Skipping LLM for channel: %s", msg.channel);
            free(msg.content);
            continue;
        }

        /* Send "working" indicator */
#if MIMI_AGENT_SEND_WORKING_STATUS
        mimi_msg_t status = {0};
        strncpy(status.channel, msg.channel, sizeof(status.channel) - 1);
        strncpy(status.chat_id, msg.chat_id, sizeof(status.chat_id) - 1);
        status.content = strdup("思考中...");
        if (status.content) {
            if (message_bus_push_outbound(&status) != ESP_OK) {
                ESP_LOGW(TAG, "Outbound queue full, drop working status");
                free(status.content);
            }
        }
#endif

        /* 1. Build system prompt */
        context_build_system_prompt(system_prompt, MIMI_CONTEXT_BUF_SIZE);
        ESP_LOGI(TAG, "LLM turn context: channel=%s chat_id=%s", msg.channel, msg.chat_id);

        /* 2. Load session history (skip on PSRAM stack — fopen FATFS) */
        if (!s_agent_psram_stack) {
            session_get_history_json(msg.chat_id, history_json,
                                    MIMI_LLM_STREAM_BUF_SIZE, MIMI_AGENT_MAX_HISTORY);
        }

        cJSON *messages = cJSON_Parse(history_json);
        if (!messages) {
            messages = cJSON_CreateArray();
        }

        agent_log_runtime_resources("after prompt build");

        /* 3. Append current user message with runtime context */
        {
            char runtime_ctx[512];
            context_build_runtime_context(runtime_ctx, sizeof(runtime_ctx), msg.channel, msg.chat_id);

            cJSON *user_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(user_msg, "role", "user");
            size_t combined_len = strlen(runtime_ctx) + 2 + strlen(msg.content ? msg.content : "") + 1;
            char *combined = heap_caps_malloc(combined_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!combined) {
                combined = heap_caps_malloc(combined_len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            }
            if (combined) {
                snprintf(combined, combined_len, "%s\n\n%s", runtime_ctx, msg.content ? msg.content : "");
                cJSON_AddStringToObject(user_msg, "content", combined);
                free(combined);
            } else {
                ESP_LOGW(TAG, "No heap for user message context, using raw content");
                cJSON_AddStringToObject(user_msg, "content", msg.content ? msg.content : "");
            }
            cJSON_AddItemToArray(messages, user_msg);
        }

        /* 4. Build run spec and execute via runner */
        AgentRunSpec spec = {
            .system_prompt = system_prompt,
            .initial_messages = messages,
            .tools_json = tools_json,
            .max_iterations = MIMI_AGENT_MAX_TOOL_ITER,
            .max_tool_result_chars = 8 * 1024,
            .error_message = "Sorry, I encountered an error.",
            .concurrent_tools = false,
            .current_msg = &msg,
            .user_intent = msg.content,
        };

        /* Check and run consolidation before LLM call.
           Skip on PSRAM stack — flash writes disable cache and assert. */
        if (!s_agent_psram_stack) {
            consolidator_check_and_run(msg.chat_id);
        }

        agent_log_runtime_resources("before LLM run");

        unsigned stack_free = (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
        if (stack_free < 2048) {
            ESP_LOGE(TAG, "Insufficient stack for LLM (stack_free=%u), aborting turn", stack_free);
            mimi_msg_t out = {0};
            strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
            strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
            out.content = strdup("Sorry, I ran out of memory.");
            if (out.content && message_bus_push_outbound(&out) != ESP_OK) {
                free(out.content);
            }
            cJSON_Delete(messages);
            free(msg.content);
            continue;
        }

        AgentRunResult *result = heap_caps_malloc(sizeof(AgentRunResult), MALLOC_CAP_SPIRAM);
        if (!result) {
            ESP_LOGE(TAG, "Failed to allocate AgentRunResult");
            free(msg.content);
            cJSON_Delete(messages);
            continue;
        }
        memset(result, 0, sizeof(AgentRunResult));
        err = agent_runner_run(&spec, result);
        agent_log_runtime_resources("after LLM run");

        /* 5. Handle result */
        if (err != ESP_OK || !result->final_content || !result->final_content[0]) {
            /* Error response */
            result->task_success = false;
            mimi_msg_t out = {0};
            strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
            strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
            out.content = strdup("Sorry, I encountered an error.");
            if (out.content && message_bus_push_outbound(&out) != ESP_OK) {
                free(out.content);
            }
        } else {
            /* Evaluate task success using learning hooks */
            bool task_success = learning_hook_evaluate(result->final_content, result->tool_sequence_json, result->stop_reason);
            result->task_success = task_success;

            /* Save to session (skip on PSRAM stack — flash write would assert) */
            if (!s_agent_psram_stack) {
                session_append(msg.chat_id, "user", msg.content);
                session_append(msg.chat_id, "assistant", result->final_content);
            }

            /* Push response to outbound */
            mimi_msg_t out = {0};
            strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
            strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
            out.content = result->final_content;  /* transfer ownership */
            ESP_LOGI(TAG, "Queue final response to %s:%s (%d bytes)",
                     out.channel, out.chat_id, (int)strlen(out.content));
            if (message_bus_push_outbound(&out) != ESP_OK) {
                ESP_LOGW(TAG, "Outbound queue full, drop final response");
                free(out.content);
            }
            result->final_content = NULL;  /* prevent double-free */
        }

        /* Call learning hooks on task end (skip on PSRAM stack) */
        if (!s_agent_psram_stack) {
            learning_hook_on_task_end(msg.chat_id, result);
        }

        /* Cleanup */
        cJSON_Delete(messages);
        if (result->messages) cJSON_Delete(result->messages);
        if (result->final_content) free(result->final_content);
        if (result->error) free(result->error);
        free(result);
        free(msg.content);

        /* Log memory status */
        ESP_LOGI(TAG, "Free PSRAM: %d bytes",
                 (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}
