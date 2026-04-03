/*
 * Stubs for plan_executor unit tests — provides symbols for tool_registry
 * and llm_tools functions that plan_executor.c references but that aren't
 * needed for the pure-logic tests (set, log, if, loop, conditions, vars).
 */

#include "llm/llm_tools.h"
#include "tools/tool_registry.h"

const tool_metadata_t *tool_registry_lookup(const char *name) {
   (void)name;
   return NULL; /* No tools registered in test */
}

bool tool_registry_is_enabled(const char *name) {
   (void)name;
   return false;
}

int llm_tools_execute(const tool_call_t *call, tool_result_t *result) {
   (void)call;
   if (result) {
      result->success = false;
      snprintf(result->result, sizeof(result->result), "Error: stub — no tools in test");
   }
   return 0;
}

/* tool_result_content() is static inline in llm_tools.h — no stub needed */

/* Session manager stubs — only needed when ENABLE_MULTI_CLIENT is defined
 * (WEBUI=ON). When WEBUI=OFF, session_manager.h provides static inline stubs. */
#include "core/session_manager.h"

#ifdef ENABLE_MULTI_CLIENT
session_t *session_get_command_context(void) {
   return NULL;
}
#endif

void webui_broadcast_plan_progress(session_t *s, const char *json) {
   (void)s;
   (void)json;
}
