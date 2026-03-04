/**
 * DAWN Memory Panel Module
 * Manages memory viewing, searching, and deletion
 */
(function () {
   'use strict';

   /* =============================================================================
    * Constants
    * ============================================================================= */

   const PAGE_SIZE = 20;

   /* =============================================================================
    * State
    * ============================================================================= */

   let memoryState = {
      stats: null,
      facts: [],
      preferences: [],
      allPreferences: [], // Keep unfiltered copy for client-side search
      summaries: [],
      entities: [],
      allEntities: [], // Keep unfiltered copy for client-side search
      activeTab: 'facts',
      searchQuery: '',
      searchTimeout: null,
      tabOffset: { facts: 0, preferences: 0, summaries: 0, entities: 0 },
      tabHasMore: { facts: false, preferences: false, summaries: false, entities: false },
      loading: false,
   };

   // Callbacks for shared utilities
   let callbacks = {
      showConfirmModal: null,
      showInputModal: null,
      getAuthState: null,
      trapFocus: null,
   };

   // Focus management state
   let focusTrapCleanup = null;
   let triggerElement = null;

   /* =============================================================================
    * Elements
    * ============================================================================= */

   const memoryElements = {
      btn: null,
      popover: null,
      closeBtn: null,
      searchInput: null,
      list: null,
      forgetAllBtn: null,
      loadMoreBtn: null,
      tabs: null,
      statFacts: null,
      statPrefs: null,
      statSummaries: null,
      statEntities: null,
   };

   /* =============================================================================
    * API Requests
    * ============================================================================= */

   function requestStats() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({ type: 'get_memory_stats' });
   }

   function requestFacts(offset) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      memoryState.loading = true;
      DawnWS.send({
         type: 'list_memory_facts',
         payload: { limit: PAGE_SIZE, offset: offset || 0 },
      });
   }

   function requestPreferences(offset) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      memoryState.loading = true;
      DawnWS.send({
         type: 'list_memory_preferences',
         payload: { limit: PAGE_SIZE, offset: offset || 0 },
      });
   }

   function requestSummaries(offset) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      memoryState.loading = true;
      DawnWS.send({
         type: 'list_memory_summaries',
         payload: { limit: PAGE_SIZE, offset: offset || 0 },
      });
   }

   function requestEntities(offset) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      memoryState.loading = true;
      DawnWS.send({
         type: 'list_memory_entities',
         payload: { limit: PAGE_SIZE, offset: offset || 0 },
      });
   }

   function requestDeleteEntity(entityId) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({
         type: 'delete_memory_entity',
         payload: { entity_id: entityId },
      });
   }

   function requestSearch(query) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      if (!query || query.trim().length === 0) return;
      memoryState.loading = true;
      DawnWS.send({
         type: 'search_memory',
         payload: { query: query.trim() },
      });
   }

   function requestDeleteFact(factId) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({
         type: 'delete_memory_fact',
         payload: { fact_id: factId },
      });
   }

   function requestDeletePreference(category) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({
         type: 'delete_memory_preference',
         payload: { category: category },
      });
   }

   function requestDeleteSummary(summaryId) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({
         type: 'delete_memory_summary',
         payload: { summary_id: summaryId },
      });
   }

   function requestDeleteAll() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({
         type: 'delete_all_memories',
         payload: { confirm: 'DELETE' },
      });
   }

   /* =============================================================================
    * Response Handlers
    * ============================================================================= */

   function handleStatsResponse(payload) {
      if (!payload.success) {
         console.error('Failed to get memory stats:', payload.error);
         return;
      }

      memoryState.stats = payload;
      updateStatsDisplay();
   }

   function handleEntitiesResponse(payload) {
      memoryState.loading = false;

      if (!payload.success) {
         console.error('Failed to list memory entities:', payload.error);
         showEmptyState('Failed to load entities');
         return;
      }

      const newEntities = payload.entities || [];
      memoryState.tabHasMore.entities = payload.has_more || false;

      if (memoryState.tabOffset.entities > 0 && newEntities.length > 0) {
         memoryState.entities = memoryState.entities.concat(newEntities);
         memoryState.allEntities = memoryState.allEntities.concat(newEntities);
         if (memoryState.activeTab === 'entities' && !memoryState.searchQuery) {
            appendEntitiesToList(newEntities);
         }
      } else {
         memoryState.entities = newEntities;
         memoryState.allEntities = newEntities;
         if (memoryState.activeTab === 'entities' && !memoryState.searchQuery) {
            renderEntitiesList();
         }
      }
   }

   function handleDeleteEntityResponse(payload) {
      if (!payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload.error || 'Failed to delete entity', 'error');
         }
         return;
      }

      if (typeof DawnToast !== 'undefined') {
         DawnToast.show('Entity deleted', 'success');
      }

      requestStats();
      memoryState.tabOffset.entities = 0;
      requestEntities(0);
   }

   function handleFactsResponse(payload) {
      memoryState.loading = false;

      if (!payload.success) {
         console.error('Failed to list memory facts:', payload.error);
         showEmptyState('Failed to load facts');
         return;
      }

      const newFacts = payload.facts || [];
      memoryState.tabHasMore.facts = payload.has_more || false;

      // If offset > 0, append to DOM efficiently; otherwise full render
      if (memoryState.tabOffset.facts > 0 && newFacts.length > 0) {
         memoryState.facts = memoryState.facts.concat(newFacts);
         if (memoryState.activeTab === 'facts' && !memoryState.searchQuery) {
            appendFactsToList(newFacts);
         }
      } else {
         memoryState.facts = newFacts;
         if (memoryState.activeTab === 'facts' && !memoryState.searchQuery) {
            renderFactsList();
         }
      }
   }

   /**
    * Append new facts to the list without rebuilding entire DOM
    */
   function appendFactsToList(newFacts) {
      if (!memoryElements.list || newFacts.length === 0) return;

      const html = newFacts.map((fact) => renderFactItem(fact)).join('');
      memoryElements.list.insertAdjacentHTML('beforeend', html);

      // Update load more button state
      if (memoryElements.loadMoreBtn) {
         memoryElements.loadMoreBtn.disabled =
            !memoryState.tabHasMore.facts || !!memoryState.searchQuery;
      }
   }

   function appendPrefsToList(newPrefs) {
      if (!memoryElements.list || newPrefs.length === 0) return;

      const html = newPrefs.map((pref) => renderPreferenceItem(pref)).join('');
      memoryElements.list.insertAdjacentHTML('beforeend', html);

      if (memoryElements.loadMoreBtn) {
         memoryElements.loadMoreBtn.disabled =
            !memoryState.tabHasMore.preferences || !!memoryState.searchQuery;
      }
   }

   function appendSummariesToList(newSummaries) {
      if (!memoryElements.list || newSummaries.length === 0) return;

      const html = newSummaries.map((summary) => renderSummaryItem(summary)).join('');
      memoryElements.list.insertAdjacentHTML('beforeend', html);

      if (memoryElements.loadMoreBtn) {
         memoryElements.loadMoreBtn.disabled =
            !memoryState.tabHasMore.summaries || !!memoryState.searchQuery;
      }
   }

   function appendEntitiesToList(newEntities) {
      if (!memoryElements.list || newEntities.length === 0) return;

      const html = newEntities.map((entity) => renderEntityItem(entity)).join('');
      memoryElements.list.insertAdjacentHTML('beforeend', html);

      if (memoryElements.loadMoreBtn) {
         memoryElements.loadMoreBtn.disabled =
            !memoryState.tabHasMore.entities || !!memoryState.searchQuery;
      }
   }

   function handlePreferencesResponse(payload) {
      memoryState.loading = false;

      if (!payload.success) {
         console.error('Failed to list memory preferences:', payload.error);
         showEmptyState('Failed to load preferences');
         return;
      }

      const newPrefs = payload.preferences || [];
      memoryState.tabHasMore.preferences = payload.has_more || false;

      if (memoryState.tabOffset.preferences > 0 && newPrefs.length > 0) {
         memoryState.preferences = memoryState.preferences.concat(newPrefs);
         memoryState.allPreferences = memoryState.allPreferences.concat(newPrefs);
         if (memoryState.activeTab === 'preferences' && !memoryState.searchQuery) {
            appendPrefsToList(newPrefs);
         }
      } else {
         memoryState.preferences = newPrefs;
         memoryState.allPreferences = newPrefs;
         if (memoryState.activeTab === 'preferences' && !memoryState.searchQuery) {
            renderPreferencesList();
         }
      }
   }

   function handleSummariesResponse(payload) {
      memoryState.loading = false;

      if (!payload.success) {
         console.error('Failed to list memory summaries:', payload.error);
         showEmptyState('Failed to load summaries');
         return;
      }

      const newSummaries = payload.summaries || [];
      memoryState.tabHasMore.summaries = payload.has_more || false;

      if (memoryState.tabOffset.summaries > 0 && newSummaries.length > 0) {
         memoryState.summaries = memoryState.summaries.concat(newSummaries);
         if (memoryState.activeTab === 'summaries' && !memoryState.searchQuery) {
            appendSummariesToList(newSummaries);
         }
      } else {
         memoryState.summaries = newSummaries;
         if (memoryState.activeTab === 'summaries' && !memoryState.searchQuery) {
            renderSummariesList();
         }
      }
   }

   function handleSearchResponse(payload) {
      memoryState.loading = false;

      if (!payload.success) {
         console.error('Failed to search memory:', payload.error);
         showEmptyState('Search failed');
         return;
      }

      // Store search results
      memoryState.facts = payload.facts || [];
      memoryState.summaries = payload.summaries || [];

      // Filter preferences client-side (not searched server-side)
      const query = memoryState.searchQuery.toLowerCase();
      memoryState.preferences = memoryState.allPreferences.filter(
         (p) =>
            (p.category && p.category.toLowerCase().includes(query)) ||
            (p.value && p.value.toLowerCase().includes(query))
      );

      // Filter entities client-side
      memoryState.entities = memoryState.allEntities.filter(
         (e) =>
            (e.name && e.name.toLowerCase().includes(query)) ||
            (e.entity_type && e.entity_type.toLowerCase().includes(query))
      );

      // Render search results based on active tab
      if (memoryState.activeTab === 'facts') {
         renderFactsList();
      } else if (memoryState.activeTab === 'preferences') {
         renderPreferencesList();
      } else if (memoryState.activeTab === 'summaries') {
         renderSummariesList();
      } else if (memoryState.activeTab === 'entities') {
         renderEntitiesList();
      }

      // Disable load more button during search
      if (memoryElements.loadMoreBtn) {
         memoryElements.loadMoreBtn.disabled = true;
      }
   }

   function handleDeleteFactResponse(payload) {
      if (!payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload.error || 'Failed to delete fact', 'error');
         }
         return;
      }

      if (typeof DawnToast !== 'undefined') {
         DawnToast.show('Fact deleted', 'success');
      }

      // Refresh data
      requestStats();
      memoryState.tabOffset.facts = 0;
      requestFacts(0);
   }

   function handleDeletePreferenceResponse(payload) {
      if (!payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload.error || 'Failed to delete preference', 'error');
         }
         return;
      }

      if (typeof DawnToast !== 'undefined') {
         DawnToast.show('Preference deleted', 'success');
      }

      // Refresh data
      requestStats();
      memoryState.tabOffset.preferences = 0;
      requestPreferences(0);
   }

   function handleDeleteSummaryResponse(payload) {
      if (!payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload.error || 'Failed to delete summary', 'error');
         }
         return;
      }

      if (typeof DawnToast !== 'undefined') {
         DawnToast.show('Summary deleted', 'success');
      }

      // Refresh data
      requestStats();
      memoryState.tabOffset.summaries = 0;
      requestSummaries(0);
   }

   function handleDeleteAllResponse(payload) {
      if (!payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload.error || 'Failed to delete memories', 'error');
         }
         return;
      }

      if (typeof DawnToast !== 'undefined') {
         DawnToast.show('All memories deleted', 'success');
      }

      // Clear local state and refresh
      memoryState.facts = [];
      memoryState.preferences = [];
      memoryState.summaries = [];
      memoryState.entities = [];
      memoryState.allEntities = [];
      memoryState.tabOffset = { facts: 0, preferences: 0, summaries: 0, entities: 0 };
      memoryState.tabHasMore = {
         facts: false,
         preferences: false,
         summaries: false,
         entities: false,
      };
      requestStats();
      showEmptyState('No memories yet');
   }

   /* =============================================================================
    * UI Update Functions
    * ============================================================================= */

   function updateStatsDisplay() {
      if (!memoryState.stats) return;

      if (memoryElements.statFacts) {
         memoryElements.statFacts.textContent = memoryState.stats.fact_count || 0;
      }
      if (memoryElements.statPrefs) {
         memoryElements.statPrefs.textContent = memoryState.stats.pref_count || 0;
      }
      if (memoryElements.statSummaries) {
         memoryElements.statSummaries.textContent = memoryState.stats.summary_count || 0;
      }
      if (memoryElements.statEntities) {
         memoryElements.statEntities.textContent = memoryState.stats.entity_count || 0;
      }
   }

   function showEmptyState(message) {
      if (!memoryElements.list) return;
      memoryElements.list.innerHTML = `<div class="memory-empty">${escapeHtml(message)}</div>`;
      if (memoryElements.loadMoreBtn) {
         memoryElements.loadMoreBtn.disabled = true;
      }
   }

   function showLoading() {
      if (!memoryElements.list) return;
      memoryElements.list.innerHTML = '<div class="memory-loading">Loading...</div>';
   }

   function renderFactsList() {
      if (!memoryElements.list) return;

      if (memoryState.facts.length === 0) {
         showEmptyState(memoryState.searchQuery ? 'No facts found' : 'No facts stored yet');
         return;
      }

      const html = memoryState.facts.map((fact) => renderFactItem(fact)).join('');
      memoryElements.list.innerHTML = html;

      // Update load more button - enable if more available
      if (memoryElements.loadMoreBtn) {
         memoryElements.loadMoreBtn.classList.remove('hidden');
         memoryElements.loadMoreBtn.disabled =
            !memoryState.tabHasMore.facts || !!memoryState.searchQuery;
      }
   }

   function renderFactItem(fact) {
      const confidence = fact.confidence || 0;
      let confidenceClass = 'confidence-low';
      if (confidence >= 0.8) confidenceClass = 'confidence-high';
      else if (confidence >= 0.5) confidenceClass = 'confidence-medium';

      const dateStr = formatDate(fact.created_at);
      const confidencePercent = Math.round(confidence * 100);

      return `
         <div class="memory-item fact" data-fact-id="${fact.id}">
            <div class="memory-item-text">${escapeHtml(fact.fact_text)}</div>
            <div class="memory-item-meta">
               <span class="memory-item-confidence ${confidenceClass}">${confidencePercent}%</span>
               <span class="memory-item-source">${escapeHtml(fact.source || 'unknown')}</span>
               <span class="memory-item-date">${dateStr}</span>
            </div>
            <button class="memory-item-delete" data-fact-id="${fact.id}" title="Delete this fact" aria-label="Delete fact">
               <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                  <path d="M3 6h18M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"/>
               </svg>
            </button>
         </div>
      `;
   }

   function renderPreferencesList() {
      if (!memoryElements.list) return;

      if (memoryState.preferences.length === 0) {
         showEmptyState('No preferences stored yet');
         return;
      }

      const html = memoryState.preferences.map((pref) => renderPreferenceItem(pref)).join('');
      memoryElements.list.innerHTML = html;

      if (memoryElements.loadMoreBtn) {
         memoryElements.loadMoreBtn.classList.remove('hidden');
         memoryElements.loadMoreBtn.disabled =
            !memoryState.tabHasMore.preferences || !!memoryState.searchQuery;
      }
   }

   function renderPreferenceItem(pref) {
      const confidence = pref.confidence || 0;
      let confidenceClass = 'confidence-low';
      if (confidence >= 0.8) confidenceClass = 'confidence-high';
      else if (confidence >= 0.5) confidenceClass = 'confidence-medium';

      const dateStr = formatDate(pref.updated_at || pref.created_at);
      const confidencePercent = Math.round(confidence * 100);

      return `
         <div class="memory-item preference" data-pref-category="${escapeHtml(pref.category)}">
            <div class="memory-item-category">${escapeHtml(pref.category)}</div>
            <div class="memory-item-value">${escapeHtml(pref.value)}</div>
            <div class="memory-item-meta">
               <span class="memory-item-confidence ${confidenceClass}">${confidencePercent}%</span>
               <span class="memory-item-source">${escapeHtml(pref.source || 'unknown')}</span>
               <span class="memory-item-date">${dateStr}</span>
               ${pref.reinforcement_count > 1 ? `<span>${pref.reinforcement_count}x</span>` : ''}
            </div>
            <button class="memory-item-delete" data-pref-category="${escapeHtml(pref.category)}" title="Delete this preference" aria-label="Delete preference">
               <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                  <path d="M3 6h18M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"/>
               </svg>
            </button>
         </div>
      `;
   }

   function renderSummariesList() {
      if (!memoryElements.list) return;

      if (memoryState.summaries.length === 0) {
         showEmptyState(
            memoryState.searchQuery ? 'No summaries found' : 'No conversation summaries yet'
         );
         return;
      }

      const html = memoryState.summaries.map((summary) => renderSummaryItem(summary)).join('');
      memoryElements.list.innerHTML = html;

      if (memoryElements.loadMoreBtn) {
         memoryElements.loadMoreBtn.classList.remove('hidden');
         memoryElements.loadMoreBtn.disabled =
            !memoryState.tabHasMore.summaries || !!memoryState.searchQuery;
      }
   }

   function renderSummaryItem(summary) {
      const dateStr = formatDate(summary.created_at);

      // Parse topics (comma-separated)
      const topics = (summary.topics || '')
         .split(',')
         .map((t) => t.trim())
         .filter((t) => t.length > 0);
      const topicsHtml = topics
         .slice(0, 5)
         .map((t) => `<span class="memory-item-topic">${escapeHtml(t)}</span>`)
         .join('');

      return `
         <div class="memory-item summary" data-summary-id="${summary.id}">
            <div class="memory-item-text">${escapeHtml(summary.summary)}</div>
            ${topicsHtml ? `<div class="memory-item-topics">${topicsHtml}</div>` : ''}
            <div class="memory-item-meta">
               <span class="memory-item-date">${dateStr}</span>
               ${summary.message_count ? `<span>${summary.message_count} messages</span>` : ''}
            </div>
            <button class="memory-item-delete" data-summary-id="${summary.id}" title="Delete this summary" aria-label="Delete summary">
               <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                  <path d="M3 6h18M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"/>
               </svg>
            </button>
         </div>
      `;
   }

   /* =============================================================================
    * Entity Rendering
    * ============================================================================= */

   const ENTITY_TYPE_CLASSES = {
      person: 'entity-type-person',
      pet: 'entity-type-pet',
      project: 'entity-type-project',
      device: 'entity-type-device',
      place: 'entity-type-place',
      organization: 'entity-type-organization',
   };

   function renderEntitiesList() {
      if (!memoryElements.list) return;

      if (memoryState.entities.length === 0) {
         showEmptyState(
            memoryState.searchQuery ? 'No entities found' : 'No entities discovered yet'
         );
         return;
      }

      const html = memoryState.entities.map((entity) => renderEntityItem(entity)).join('');
      memoryElements.list.innerHTML = html;

      if (memoryElements.loadMoreBtn) {
         memoryElements.loadMoreBtn.classList.remove('hidden');
         memoryElements.loadMoreBtn.disabled =
            !memoryState.tabHasMore.entities || !!memoryState.searchQuery;
      }
   }

   function renderEntityItem(entity) {
      const typeClass = ENTITY_TYPE_CLASSES[entity.entity_type] || 'entity-type-default';
      const dateStr = formatDate(entity.first_seen);
      const relations = entity.relations || [];
      const maxVisible = 3;
      const hasMore = relations.length > maxVisible;
      const visible = hasMore ? relations.slice(0, maxVisible) : relations;

      const relationsHtml = visible.map((rel) => renderRelationLine(rel)).join('');

      const moreHtml = hasMore
         ? `<div class="entity-relations-more" data-expand-entity="${entity.id}" tabindex="0" role="button">` +
           `+${relations.length - maxVisible} more</div>`
         : '';

      const hiddenHtml = hasMore
         ? `<div class="entity-relations-hidden" data-entity-hidden="${entity.id}" style="display:none">` +
           relations
              .slice(maxVisible)
              .map((rel) => renderRelationLine(rel))
              .join('') +
           '</div>'
         : '';

      return (
         `<div class="memory-item entity" data-entity-id="${entity.id}">` +
         `<div class="entity-header">` +
         `<span class="entity-type-badge ${typeClass}">${escapeHtml(entity.entity_type || 'other')}</span>` +
         `<span class="entity-name">${escapeHtml(entity.name)}</span>` +
         `</div>` +
         `<div class="memory-item-meta">` +
         `<span class="entity-mentions">${entity.mention_count || 0} mentions</span>` +
         (dateStr ? `<span class="memory-item-date">${dateStr}</span>` : '') +
         `</div>` +
         (relations.length > 0
            ? `<div class="entity-relations">${relationsHtml}${hiddenHtml}${moreHtml}</div>`
            : '') +
         `<button class="memory-item-delete" data-entity-id="${entity.id}" ` +
         `title="Delete this entity" aria-label="Delete entity">` +
         `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">` +
         `<path d="M3 6h18M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"/>` +
         `</svg></button></div>`
      );
   }

   function renderRelationLine(rel) {
      const isLinked = rel.object_entity_id && rel.object_entity_id > 0;
      const targetClass = isLinked ? 'entity-relation-target' : 'entity-relation-value';
      const targetAttr = isLinked
         ? ` data-target-entity="${rel.object_entity_id}" tabindex="0" role="button"`
         : '';
      const targetName = escapeHtml(rel.object_name || '');
      const dirIndicator =
         rel.direction === 'in' ? '<span class="entity-relation-arrow">&larr;</span>' : '';

      const arrow = rel.direction === 'in' ? '&larr;' : '&rarr;';

      return (
         `<div class="entity-relation">` +
         `<span class="entity-relation-arrow">${arrow}</span>` +
         `<span class="entity-relation-verb">${escapeHtml(rel.relation)}</span> ` +
         `<span class="${targetClass}"${targetAttr}>${targetName}</span>` +
         `</div>`
      );
   }

   /**
    * Set up event delegation for delete buttons (called once in init)
    */
   function setupDeleteDelegation() {
      if (!memoryElements.list) return;
      memoryElements.list.addEventListener('click', handleListClick);
      memoryElements.list.addEventListener('keydown', handleListKeydown);
   }

   function handleListKeydown(e) {
      if (e.key !== 'Enter' && e.key !== ' ') return;
      const target = e.target.closest('.entity-relation-target, .entity-relations-more');
      if (!target) return;
      e.preventDefault();
      target.click();
   }

   function handleListClick(e) {
      // Handle relation target clicks (scroll to entity)
      const relTarget = e.target.closest('.entity-relation-target');
      if (relTarget) {
         const targetId = relTarget.dataset.targetEntity;
         if (targetId) {
            scrollToEntity(parseInt(targetId, 10));
         }
         return;
      }

      // Handle "+N more" expand toggle
      const moreToggle = e.target.closest('.entity-relations-more');
      if (moreToggle) {
         const entityId = moreToggle.dataset.expandEntity;
         const hidden = memoryElements.list.querySelector(`[data-entity-hidden="${entityId}"]`);
         if (hidden) {
            const isVisible = hidden.style.display !== 'none';
            hidden.style.display = isVisible ? 'none' : '';
            moreToggle.textContent = isVisible ? `+${hidden.children.length} more` : 'show less';
         }
         return;
      }

      const btn = e.target.closest('.memory-item-delete');
      if (!btn) return;

      e.stopPropagation();
      const factId = btn.dataset.factId;
      const prefCategory = btn.dataset.prefCategory;
      const summaryId = btn.dataset.summaryId;
      const entityId = btn.dataset.entityId;

      if (factId) {
         const fact = memoryState.facts.find((f) => f.id === parseInt(factId, 10));
         const detail = fact ? fact.fact_text : null;

         if (callbacks.showConfirmModal) {
            callbacks.showConfirmModal(
               'Delete this fact?',
               () => {
                  requestDeleteFact(parseInt(factId, 10));
               },
               { detail: detail, danger: true }
            );
         } else {
            requestDeleteFact(parseInt(factId, 10));
         }
      } else if (prefCategory) {
         const pref = memoryState.preferences.find((p) => p.category === prefCategory);
         const detail = pref ? `${pref.category}: ${pref.value}` : null;

         if (callbacks.showConfirmModal) {
            callbacks.showConfirmModal(
               'Delete this preference?',
               () => {
                  requestDeletePreference(prefCategory);
               },
               { detail: detail, danger: true }
            );
         } else {
            requestDeletePreference(prefCategory);
         }
      } else if (summaryId) {
         const summary = memoryState.summaries.find((s) => s.id === parseInt(summaryId, 10));
         const detail = summary ? summary.summary : null;

         if (callbacks.showConfirmModal) {
            callbacks.showConfirmModal(
               'Delete this summary?',
               () => {
                  requestDeleteSummary(parseInt(summaryId, 10));
               },
               { detail: detail, danger: true }
            );
         } else {
            requestDeleteSummary(parseInt(summaryId, 10));
         }
      } else if (entityId) {
         const entity = memoryState.entities.find((e) => e.id === parseInt(entityId, 10));
         const detail = entity ? `${entity.name} (${entity.entity_type})` : null;

         if (callbacks.showConfirmModal) {
            callbacks.showConfirmModal(
               'Delete this entity and its relations?',
               () => {
                  requestDeleteEntity(parseInt(entityId, 10));
               },
               { detail: detail, danger: true }
            );
         } else {
            requestDeleteEntity(parseInt(entityId, 10));
         }
      }
   }

   function scrollToEntity(entityId) {
      if (!memoryElements.list) return;
      const el = memoryElements.list.querySelector(
         `.memory-item.entity[data-entity-id="${entityId}"]`
      );
      if (el) {
         el.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
         el.classList.remove('highlight-pulse');
         // Force reflow to restart animation
         void el.offsetWidth;
         el.classList.add('highlight-pulse');
      }
   }

   /* =============================================================================
    * Panel Control
    * ============================================================================= */

   function open() {
      if (!memoryElements.popover) return;

      // Store trigger element for focus restoration
      triggerElement = document.activeElement;

      memoryElements.popover.classList.remove('hidden');
      memoryElements.btn.classList.add('active');

      // Request fresh data
      requestStats();
      loadActiveTabData();

      // Set up focus trap
      if (callbacks.trapFocus) {
         focusTrapCleanup = callbacks.trapFocus(memoryElements.popover);
      } else if (memoryElements.searchInput) {
         // Fallback: just focus search input
         setTimeout(() => memoryElements.searchInput.focus(), 100);
      }

      // Add click-outside listener
      document.addEventListener('click', handleClickOutside);
      document.addEventListener('keydown', handleKeyDown);
   }

   function close() {
      if (!memoryElements.popover) return;

      memoryElements.popover.classList.add('hidden');
      memoryElements.btn.classList.remove('active');

      // Clear search
      if (memoryElements.searchInput) {
         memoryElements.searchInput.value = '';
      }
      memoryState.searchQuery = '';

      // Clean up focus trap
      if (focusTrapCleanup) {
         focusTrapCleanup();
         focusTrapCleanup = null;
      }

      // Restore focus to trigger element
      if (triggerElement && typeof triggerElement.focus === 'function') {
         triggerElement.focus();
         triggerElement = null;
      }

      // Remove listeners
      document.removeEventListener('click', handleClickOutside);
      document.removeEventListener('keydown', handleKeyDown);
   }

   function toggle() {
      if (memoryElements.popover && memoryElements.popover.classList.contains('hidden')) {
         open();
      } else {
         close();
      }
   }

   function handleClickOutside(e) {
      if (
         memoryElements.popover &&
         !memoryElements.popover.contains(e.target) &&
         memoryElements.btn &&
         !memoryElements.btn.contains(e.target)
      ) {
         close();
      }
   }

   function handleKeyDown(e) {
      if (e.key === 'Escape') {
         close();
      }
   }

   /* =============================================================================
    * Tab Handling
    * ============================================================================= */

   function switchTab(tabName) {
      if (memoryState.activeTab === tabName) return;

      memoryState.activeTab = tabName;

      // Update tab buttons
      if (memoryElements.tabs) {
         memoryElements.tabs.forEach((tab) => {
            if (tab.dataset.tab === tabName) {
               tab.classList.add('active');
               tab.setAttribute('aria-selected', 'true');
            } else {
               tab.classList.remove('active');
               tab.setAttribute('aria-selected', 'false');
            }
         });
      }

      // Update tabpanel aria-labelledby to point to active tab
      if (memoryElements.list) {
         memoryElements.list.setAttribute('aria-labelledby', 'memory-tab-' + tabName);
      }

      // Clear search and load data
      if (memoryElements.searchInput) {
         memoryElements.searchInput.value = '';
      }
      memoryState.searchQuery = '';

      loadActiveTabData();
   }

   function loadActiveTabData() {
      showLoading();

      // Disable load more while loading
      if (memoryElements.loadMoreBtn) {
         memoryElements.loadMoreBtn.classList.remove('hidden');
         memoryElements.loadMoreBtn.disabled = true;
      }

      const tab = memoryState.activeTab;
      memoryState.tabOffset[tab] = 0;

      switch (tab) {
         case 'facts':
            requestFacts(0);
            break;
         case 'preferences':
            requestPreferences(0);
            break;
         case 'summaries':
            requestSummaries(0);
            break;
         case 'entities':
            requestEntities(0);
            break;
      }
   }

   /* =============================================================================
    * Search Handling
    * ============================================================================= */

   function handleSearchInput() {
      const query = memoryElements.searchInput.value.trim();

      // Clear existing timeout
      if (memoryState.searchTimeout) {
         clearTimeout(memoryState.searchTimeout);
      }

      // If empty, reload tab data
      if (query.length === 0) {
         memoryState.searchQuery = '';
         loadActiveTabData();
         return;
      }

      // Debounce search
      memoryState.searchTimeout = setTimeout(() => {
         memoryState.searchQuery = query;
         showLoading();
         requestSearch(query);
      }, 300);
   }

   /* =============================================================================
    * Forget All Handling
    * ============================================================================= */

   function handleForgetAll() {
      if (callbacks.showInputModal) {
         callbacks.showInputModal(
            'Type DELETE to confirm forgetting all memories.',
            '',
            (value) => {
               if (value && value.toUpperCase() === 'DELETE') {
                  requestDeleteAll();
               } else if (value) {
                  if (typeof DawnToast !== 'undefined') {
                     DawnToast.show('You must type DELETE to confirm', 'error');
                  }
               }
            },
            { title: 'Forget Everything?', placeholder: 'DELETE' }
         );
      } else if (confirm('Are you sure you want to delete ALL memories? This cannot be undone.')) {
         requestDeleteAll();
      }
   }

   /* =============================================================================
    * Load More Handling
    * ============================================================================= */

   function handleLoadMore() {
      const tab = memoryState.activeTab;
      if (!memoryState.tabHasMore[tab]) return;
      if (memoryState.loading) return;

      memoryState.tabOffset[tab] += PAGE_SIZE;

      switch (tab) {
         case 'facts':
            requestFacts(memoryState.tabOffset[tab]);
            break;
         case 'preferences':
            requestPreferences(memoryState.tabOffset[tab]);
            break;
         case 'summaries':
            requestSummaries(memoryState.tabOffset[tab]);
            break;
         case 'entities':
            requestEntities(memoryState.tabOffset[tab]);
            break;
      }
   }

   /* =============================================================================
    * Utility Functions
    * ============================================================================= */

   function formatDate(timestamp) {
      if (!timestamp) return '';

      const date = new Date(timestamp * 1000);
      const now = new Date();
      const diffDays = Math.floor((now - date) / (1000 * 60 * 60 * 24));

      if (diffDays === 0) {
         return 'Today';
      } else if (diffDays === 1) {
         return 'Yesterday';
      } else if (diffDays < 7) {
         return date.toLocaleDateString(undefined, { weekday: 'short' });
      } else {
         return date.toLocaleDateString(undefined, { month: 'short', day: 'numeric' });
      }
   }

   function escapeHtml(text) {
      if (!text) return '';
      const div = document.createElement('div');
      div.textContent = text;
      return div.innerHTML;
   }

   /* =============================================================================
    * Visibility Control
    * ============================================================================= */

   function updateVisibility() {
      if (!memoryElements.btn) return;

      const authState = callbacks.getAuthState ? callbacks.getAuthState() : null;
      const isAuthenticated = authState && authState.authenticated;

      if (isAuthenticated) {
         memoryElements.btn.classList.remove('hidden');
      } else {
         memoryElements.btn.classList.add('hidden');
         close();
      }
   }

   /* =============================================================================
    * Initialization
    * ============================================================================= */

   function init(options) {
      // Store callbacks
      if (options) {
         callbacks.showConfirmModal = options.showConfirmModal;
         callbacks.showInputModal = options.showInputModal;
         callbacks.getAuthState = options.getAuthState;
         callbacks.trapFocus = options.trapFocus;
      }

      // Get elements
      memoryElements.btn = document.getElementById('memory-btn');
      memoryElements.popover = document.getElementById('memory-popover');
      memoryElements.closeBtn = document.getElementById('memory-close');
      memoryElements.searchInput = document.getElementById('memory-search-input');
      memoryElements.list = document.getElementById('memory-list');
      memoryElements.forgetAllBtn = document.getElementById('memory-forget-all');
      memoryElements.loadMoreBtn = document.getElementById('memory-load-more');
      memoryElements.tabs = document.querySelectorAll('.memory-tab');
      memoryElements.statFacts = document.getElementById('memory-fact-count');
      memoryElements.statPrefs = document.getElementById('memory-pref-count');
      memoryElements.statSummaries = document.getElementById('memory-summary-count');
      memoryElements.statEntities = document.getElementById('memory-entity-count');

      if (!memoryElements.btn || !memoryElements.popover) {
         console.warn('DawnMemory: Required elements not found');
         return;
      }

      // Set up event delegation for delete buttons (single listener)
      setupDeleteDelegation();

      // Button click handler
      memoryElements.btn.addEventListener('click', (e) => {
         e.stopPropagation();
         toggle();
      });

      // Close button handler
      if (memoryElements.closeBtn) {
         memoryElements.closeBtn.addEventListener('click', close);
      }

      // Tab handlers
      if (memoryElements.tabs) {
         memoryElements.tabs.forEach((tab) => {
            tab.addEventListener('click', () => switchTab(tab.dataset.tab));
         });
      }

      // Search handler
      if (memoryElements.searchInput) {
         memoryElements.searchInput.addEventListener('input', handleSearchInput);
      }

      // Forget all handler
      if (memoryElements.forgetAllBtn) {
         memoryElements.forgetAllBtn.addEventListener('click', handleForgetAll);
      }

      // Load more handler
      if (memoryElements.loadMoreBtn) {
         memoryElements.loadMoreBtn.addEventListener('click', handleLoadMore);
      }

      console.log('DawnMemory: Initialized');
   }

   /* =============================================================================
    * Export
    * ============================================================================= */

   window.DawnMemory = {
      init,
      open,
      close,
      toggle,
      updateVisibility,
      // Response handlers
      handleStatsResponse,
      handleFactsResponse,
      handlePreferencesResponse,
      handleSummariesResponse,
      handleEntitiesResponse,
      handleSearchResponse,
      handleDeleteFactResponse,
      handleDeletePreferenceResponse,
      handleDeleteSummaryResponse,
      handleDeleteEntityResponse,
      handleDeleteAllResponse,
   };
})();
