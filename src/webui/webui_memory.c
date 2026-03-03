/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * WebUI Memory Handlers - Memory management for WebSocket clients
 *
 * This module handles WebSocket messages for memory operations:
 * - get_memory_stats, list_memory_facts, list_memory_preferences
 * - list_memory_summaries, search_memory, delete_memory_fact
 * - delete_memory_preference, delete_all_memories
 */

#include <string.h>

#include "logging.h"
#include "memory/memory_db.h"
#include "webui/webui_internal.h"

/* Default pagination limits */
#define DEFAULT_MEMORY_LIMIT 20
#define MAX_MEMORY_LIMIT 50

/* =============================================================================
 * Memory Statistics Handler
 * ============================================================================= */

/**
 * @brief Get memory statistics for the current user
 */
void handle_get_memory_stats(ws_connection_t *conn) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("get_memory_stats_response"));
   json_object *resp_payload = json_object_new_object();

   memory_stats_t stats;
   int result = memory_db_get_stats(conn->auth_user_id, &stats);

   if (result == MEMORY_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "fact_count", json_object_new_int(stats.fact_count));
      json_object_object_add(resp_payload, "pref_count", json_object_new_int(stats.pref_count));
      json_object_object_add(resp_payload, "summary_count",
                             json_object_new_int(stats.summary_count));
      json_object_object_add(resp_payload, "entity_count", json_object_new_int(stats.entity_count));
      json_object_object_add(resp_payload, "oldest_fact", json_object_new_int64(stats.oldest_fact));
      json_object_object_add(resp_payload, "newest_fact", json_object_new_int64(stats.newest_fact));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to get memory stats"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/* =============================================================================
 * Memory Facts Handlers
 * ============================================================================= */

/**
 * @brief List memory facts for the current user (paginated)
 */
void handle_list_memory_facts(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("list_memory_facts_response"));
   json_object *resp_payload = json_object_new_object();

   /* Parse pagination params */
   int limit = DEFAULT_MEMORY_LIMIT;
   int offset = 0;
   if (payload) {
      json_object *limit_obj, *offset_obj;
      if (json_object_object_get_ex(payload, "limit", &limit_obj)) {
         limit = json_object_get_int(limit_obj);
         if (limit < 1 || limit > MAX_MEMORY_LIMIT) {
            limit = DEFAULT_MEMORY_LIMIT;
         }
      }
      if (json_object_object_get_ex(payload, "offset", &offset_obj)) {
         offset = json_object_get_int(offset_obj);
         if (offset < 0) {
            offset = 0;
         }
      }
   }

   /* Query database */
   memory_fact_t facts[MAX_MEMORY_LIMIT];
   memset(facts, 0, sizeof(facts));
   int count = memory_db_fact_list(conn->auth_user_id, facts, limit, offset);

   if (count >= 0) {
      json_object *facts_array = json_object_new_array();
      for (int i = 0; i < count; i++) {
         json_object *fact_obj = json_object_new_object();
         json_object_object_add(fact_obj, "id", json_object_new_int64(facts[i].id));
         json_object_object_add(fact_obj, "fact_text", json_object_new_string(facts[i].fact_text));
         json_object_object_add(fact_obj, "confidence",
                                json_object_new_double(facts[i].confidence));
         json_object_object_add(fact_obj, "source", json_object_new_string(facts[i].source));
         json_object_object_add(fact_obj, "created_at", json_object_new_int64(facts[i].created_at));
         json_object_object_add(fact_obj, "last_accessed",
                                json_object_new_int64(facts[i].last_accessed));
         json_object_object_add(fact_obj, "access_count",
                                json_object_new_int(facts[i].access_count));
         json_object_array_add(facts_array, fact_obj);
      }

      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "facts", facts_array);
      json_object_object_add(resp_payload, "count", json_object_new_int(count));
      json_object_object_add(resp_payload, "has_more", json_object_new_boolean(count == limit));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to list memory facts"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/**
 * @brief Delete a memory fact
 */
void handle_delete_memory_fact(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("delete_memory_fact_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get fact ID */
   json_object *id_obj;
   if (!json_object_object_get_ex(payload, "fact_id", &id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing fact_id"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   int64_t fact_id = json_object_get_int64(id_obj);
   int result = memory_db_fact_delete(fact_id, conn->auth_user_id);

   if (result == MEMORY_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message", json_object_new_string("Fact deleted"));
      LOG_INFO("WebUI: User %d deleted memory fact %lld", conn->auth_user_id, (long long)fact_id);
   } else if (result == MEMORY_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Fact not found"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to delete fact"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/* =============================================================================
 * Memory Preferences Handlers
 * ============================================================================= */

/**
 * @brief List memory preferences for the current user
 */
void handle_list_memory_preferences(ws_connection_t *conn) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("list_memory_preferences_response"));
   json_object *resp_payload = json_object_new_object();

   /* Query database */
   memory_preference_t prefs[MEMORY_MAX_PREFS];
   memset(prefs, 0, sizeof(prefs));
   int count = memory_db_pref_list(conn->auth_user_id, prefs, MEMORY_MAX_PREFS);

   if (count >= 0) {
      json_object *prefs_array = json_object_new_array();
      for (int i = 0; i < count; i++) {
         json_object *pref_obj = json_object_new_object();
         json_object_object_add(pref_obj, "id", json_object_new_int64(prefs[i].id));
         json_object_object_add(pref_obj, "category", json_object_new_string(prefs[i].category));
         json_object_object_add(pref_obj, "value", json_object_new_string(prefs[i].value));
         json_object_object_add(pref_obj, "confidence",
                                json_object_new_double(prefs[i].confidence));
         json_object_object_add(pref_obj, "source", json_object_new_string(prefs[i].source));
         json_object_object_add(pref_obj, "created_at", json_object_new_int64(prefs[i].created_at));
         json_object_object_add(pref_obj, "updated_at", json_object_new_int64(prefs[i].updated_at));
         json_object_object_add(pref_obj, "reinforcement_count",
                                json_object_new_int(prefs[i].reinforcement_count));
         json_object_array_add(prefs_array, pref_obj);
      }

      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "preferences", prefs_array);
      json_object_object_add(resp_payload, "count", json_object_new_int(count));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to list preferences"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/**
 * @brief Delete a memory preference by category
 */
void handle_delete_memory_preference(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("delete_memory_preference_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get category */
   json_object *cat_obj;
   if (!json_object_object_get_ex(payload, "category", &cat_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing category"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   const char *category = json_object_get_string(cat_obj);
   int result = memory_db_pref_delete(conn->auth_user_id, category);

   if (result == MEMORY_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message", json_object_new_string("Preference deleted"));
      LOG_INFO("WebUI: User %d deleted memory preference '%s'", conn->auth_user_id, category);
   } else if (result == MEMORY_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Preference not found"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to delete preference"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/* =============================================================================
 * Memory Summaries Handler
 * ============================================================================= */

/**
 * @brief List memory summaries for the current user
 */
void handle_list_memory_summaries(ws_connection_t *conn) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("list_memory_summaries_response"));
   json_object *resp_payload = json_object_new_object();

   /* Query database */
   memory_summary_t summaries[MEMORY_MAX_SUMMARIES];
   memset(summaries, 0, sizeof(summaries));
   int count = memory_db_summary_list(conn->auth_user_id, summaries, MEMORY_MAX_SUMMARIES);

   if (count >= 0) {
      json_object *summaries_array = json_object_new_array();
      for (int i = 0; i < count; i++) {
         json_object *summary_obj = json_object_new_object();
         json_object_object_add(summary_obj, "id", json_object_new_int64(summaries[i].id));
         json_object_object_add(summary_obj, "session_id",
                                json_object_new_string(summaries[i].session_id));
         json_object_object_add(summary_obj, "summary",
                                json_object_new_string(summaries[i].summary));
         json_object_object_add(summary_obj, "topics", json_object_new_string(summaries[i].topics));
         json_object_object_add(summary_obj, "sentiment",
                                json_object_new_string(summaries[i].sentiment));
         json_object_object_add(summary_obj, "created_at",
                                json_object_new_int64(summaries[i].created_at));
         json_object_object_add(summary_obj, "message_count",
                                json_object_new_int(summaries[i].message_count));
         json_object_object_add(summary_obj, "duration_seconds",
                                json_object_new_int(summaries[i].duration_seconds));
         json_object_array_add(summaries_array, summary_obj);
      }

      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "summaries", summaries_array);
      json_object_object_add(resp_payload, "count", json_object_new_int(count));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to list summaries"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/**
 * @brief Delete a memory summary
 */
void handle_delete_memory_summary(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("delete_memory_summary_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get summary ID */
   json_object *id_obj;
   if (!json_object_object_get_ex(payload, "summary_id", &id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing summary_id"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   int64_t summary_id = json_object_get_int64(id_obj);
   int result = memory_db_summary_delete(summary_id, conn->auth_user_id);

   if (result == MEMORY_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message", json_object_new_string("Summary deleted"));
      LOG_INFO("WebUI: User %d deleted memory summary %lld", conn->auth_user_id,
               (long long)summary_id);
   } else if (result == MEMORY_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Summary not found"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to delete summary"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/* =============================================================================
 * Memory Entities Handlers
 * ============================================================================= */

/**
 * @brief List memory entities with their relations for the current user
 */
void handle_list_memory_entities(ws_connection_t *conn) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("list_memory_entities_response"));
   json_object *resp_payload = json_object_new_object();

   /* Load all entities (sorted by mention_count desc) */
   memory_entity_t entities[MAX_MEMORY_LIMIT];
   memset(entities, 0, sizeof(entities));
   int count = memory_db_entity_list(conn->auth_user_id, entities, MAX_MEMORY_LIMIT);

/* Bulk-load all relations in a single query (avoids N+1 per-entity queries) */
#define MAX_RELATIONS_BULK 400
   memory_relation_t all_rels[MAX_RELATIONS_BULK];
   int total_rels = 0;
   if (count > 0) {
      memset(all_rels, 0, sizeof(all_rels));
      total_rels = memory_db_relation_list_all_by_user(conn->auth_user_id, all_rels,
                                                       MAX_RELATIONS_BULK);
      if (total_rels < 0)
         total_rels = 0;
   }

   if (count >= 0) {
      json_object *entities_array = json_object_new_array();
      for (int i = 0; i < count; i++) {
         json_object *entity_obj = json_object_new_object();
         json_object_object_add(entity_obj, "id", json_object_new_int64(entities[i].id));
         json_object_object_add(entity_obj, "name", json_object_new_string(entities[i].name));
         json_object_object_add(entity_obj, "entity_type",
                                json_object_new_string(entities[i].entity_type));
         json_object_object_add(entity_obj, "mention_count",
                                json_object_new_int(entities[i].mention_count));
         json_object_object_add(entity_obj, "first_seen",
                                json_object_new_int64(entities[i].first_seen));
         json_object_object_add(entity_obj, "last_seen",
                                json_object_new_int64(entities[i].last_seen));

         json_object *relations_array = json_object_new_array();

         /* Scan bulk relations: outgoing (this entity is subject) */
         for (int r = 0; r < total_rels; r++) {
            if (all_rels[r].subject_entity_id != entities[i].id)
               continue;
            json_object *rel_obj = json_object_new_object();
            json_object_object_add(rel_obj, "relation",
                                   json_object_new_string(all_rels[r].relation));
            json_object_object_add(rel_obj, "object_name",
                                   json_object_new_string(all_rels[r].object_name));
            json_object_object_add(rel_obj, "object_entity_id",
                                   json_object_new_int64(all_rels[r].object_entity_id));
            json_object_object_add(rel_obj, "direction", json_object_new_string("out"));
            json_object_object_add(rel_obj, "confidence",
                                   json_object_new_double(all_rels[r].confidence));
            json_object_array_add(relations_array, rel_obj);
         }

         /* Scan bulk relations: incoming (this entity is object) */
         for (int r = 0; r < total_rels; r++) {
            if (all_rels[r].object_entity_id != entities[i].id)
               continue;
            /* For incoming, find subject entity name */
            const char *subj_name = "";
            for (int e = 0; e < count; e++) {
               if (entities[e].id == all_rels[r].subject_entity_id) {
                  subj_name = entities[e].name;
                  break;
               }
            }
            json_object *rel_obj = json_object_new_object();
            json_object_object_add(rel_obj, "relation",
                                   json_object_new_string(all_rels[r].relation));
            json_object_object_add(rel_obj, "object_name", json_object_new_string(subj_name));
            json_object_object_add(rel_obj, "object_entity_id",
                                   json_object_new_int64(all_rels[r].subject_entity_id));
            json_object_object_add(rel_obj, "direction", json_object_new_string("in"));
            json_object_object_add(rel_obj, "confidence",
                                   json_object_new_double(all_rels[r].confidence));
            json_object_array_add(relations_array, rel_obj);
         }

         json_object_object_add(entity_obj, "relations", relations_array);
         json_object_array_add(entities_array, entity_obj);
      }

      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "entities", entities_array);
      json_object_object_add(resp_payload, "count", json_object_new_int(count));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to list entities"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/**
 * @brief Delete a memory entity and its relations
 */
void handle_delete_memory_entity(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("delete_memory_entity_response"));
   json_object *resp_payload = json_object_new_object();

   json_object *id_obj;
   if (!json_object_object_get_ex(payload, "entity_id", &id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing entity_id"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   int64_t entity_id = json_object_get_int64(id_obj);
   int result = memory_db_entity_delete(entity_id, conn->auth_user_id);

   if (result == MEMORY_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message", json_object_new_string("Entity deleted"));
      LOG_INFO("WebUI: User %d deleted memory entity %lld", conn->auth_user_id,
               (long long)entity_id);
   } else if (result == MEMORY_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Entity not found"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to delete entity"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/* =============================================================================
 * Memory Search Handler
 * ============================================================================= */

/**
 * @brief Search memory facts by keyword
 */
void handle_search_memory(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("search_memory_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get search query */
   json_object *query_obj;
   if (!json_object_object_get_ex(payload, "query", &query_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing query"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   const char *query = json_object_get_string(query_obj);
   if (!query || strlen(query) == 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Empty query"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   /* Limit query length to prevent resource exhaustion */
   if (strlen(query) > 256) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Query too long"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   /* Search facts */
   memory_fact_t facts[MAX_MEMORY_LIMIT];
   memset(facts, 0, sizeof(facts));
   int count = memory_db_fact_search(conn->auth_user_id, query, facts, MAX_MEMORY_LIMIT);

   /* Also search summaries */
   memory_summary_t summaries[MEMORY_MAX_SUMMARIES];
   memset(summaries, 0, sizeof(summaries));
   int summary_count = memory_db_summary_search(conn->auth_user_id, query, summaries,
                                                MEMORY_MAX_SUMMARIES);

   if (count >= 0 && summary_count >= 0) {
      /* Build facts array */
      json_object *facts_array = json_object_new_array();
      for (int i = 0; i < count; i++) {
         json_object *fact_obj = json_object_new_object();
         json_object_object_add(fact_obj, "id", json_object_new_int64(facts[i].id));
         json_object_object_add(fact_obj, "fact_text", json_object_new_string(facts[i].fact_text));
         json_object_object_add(fact_obj, "confidence",
                                json_object_new_double(facts[i].confidence));
         json_object_object_add(fact_obj, "source", json_object_new_string(facts[i].source));
         json_object_object_add(fact_obj, "created_at", json_object_new_int64(facts[i].created_at));
         json_object_array_add(facts_array, fact_obj);
      }

      /* Build summaries array */
      json_object *summaries_array = json_object_new_array();
      for (int i = 0; i < summary_count; i++) {
         json_object *summary_obj = json_object_new_object();
         json_object_object_add(summary_obj, "id", json_object_new_int64(summaries[i].id));
         json_object_object_add(summary_obj, "summary",
                                json_object_new_string(summaries[i].summary));
         json_object_object_add(summary_obj, "topics", json_object_new_string(summaries[i].topics));
         json_object_object_add(summary_obj, "created_at",
                                json_object_new_int64(summaries[i].created_at));
         json_object_array_add(summaries_array, summary_obj);
      }

      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "facts", facts_array);
      json_object_object_add(resp_payload, "summaries", summaries_array);
      json_object_object_add(resp_payload, "fact_count", json_object_new_int(count));
      json_object_object_add(resp_payload, "summary_count", json_object_new_int(summary_count));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Search failed"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}

/* =============================================================================
 * Delete All Memories Handler
 * ============================================================================= */

/**
 * @brief Delete all memories for the current user
 *
 * Requires explicit confirmation via "confirm" field in payload.
 */
void handle_delete_all_memories(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("delete_all_memories_response"));
   json_object *resp_payload = json_object_new_object();

   /* Require explicit confirmation */
   json_object *confirm_obj;
   if (!json_object_object_get_ex(payload, "confirm", &confirm_obj) ||
       strcmp(json_object_get_string(confirm_obj), "DELETE") != 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Must confirm by setting confirm=\"DELETE\""));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn->wsi, response);
      json_object_put(response);
      return;
   }

   int result = memory_db_delete_user_memories(conn->auth_user_id);

   if (result == MEMORY_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message",
                             json_object_new_string("All memories deleted"));
      LOG_INFO("WebUI: User %d deleted all memories", conn->auth_user_id);
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to delete memories"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn->wsi, response);
   json_object_put(response);
}
