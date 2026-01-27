/**
 * DAWN Memory Panel Module
 * Manages memory viewing, searching, and deletion
 */
(function () {
   'use strict';

   /* =============================================================================
    * Constants
    * ============================================================================= */

   const FACTS_PAGE_SIZE = 20;

   /* =============================================================================
    * State
    * ============================================================================= */

   let memoryState = {
      stats: null,
      facts: [],
      preferences: [],
      allPreferences: [], // Keep unfiltered copy for client-side search
      summaries: [],
      activeTab: 'facts',
      searchQuery: '',
      searchTimeout: null,
      factsOffset: 0,
      factsHasMore: false,
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
         payload: { limit: FACTS_PAGE_SIZE, offset: offset || 0 },
      });
   }

   function requestPreferences() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      memoryState.loading = true;
      DawnWS.send({ type: 'list_memory_preferences' });
   }

   function requestSummaries() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      memoryState.loading = true;
      DawnWS.send({ type: 'list_memory_summaries' });
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

   function handleFactsResponse(payload) {
      memoryState.loading = false;

      if (!payload.success) {
         console.error('Failed to list memory facts:', payload.error);
         showEmptyState('Failed to load facts');
         return;
      }

      const newFacts = payload.facts || [];
      memoryState.factsHasMore = payload.has_more || false;

      // If offset > 0, append to DOM efficiently; otherwise full render
      if (memoryState.factsOffset > 0 && newFacts.length > 0) {
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
            !memoryState.factsHasMore || !!memoryState.searchQuery;
      }
   }

   function handlePreferencesResponse(payload) {
      memoryState.loading = false;

      if (!payload.success) {
         console.error('Failed to list memory preferences:', payload.error);
         showEmptyState('Failed to load preferences');
         return;
      }

      memoryState.preferences = payload.preferences || [];
      memoryState.allPreferences = payload.preferences || [];

      if (memoryState.activeTab === 'preferences' && !memoryState.searchQuery) {
         renderPreferencesList();
      }
   }

   function handleSummariesResponse(payload) {
      memoryState.loading = false;

      if (!payload.success) {
         console.error('Failed to list memory summaries:', payload.error);
         showEmptyState('Failed to load summaries');
         return;
      }

      memoryState.summaries = payload.summaries || [];

      if (memoryState.activeTab === 'summaries' && !memoryState.searchQuery) {
         renderSummariesList();
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

      // Render search results based on active tab
      if (memoryState.activeTab === 'facts') {
         renderFactsList();
      } else if (memoryState.activeTab === 'preferences') {
         renderPreferencesList();
      } else if (memoryState.activeTab === 'summaries') {
         renderSummariesList();
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
      memoryState.factsOffset = 0;
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
      requestPreferences();
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
      requestSummaries();
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
      memoryState.factsOffset = 0;
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

      // Update load more button - show on facts tab, enable if more available
      if (memoryElements.loadMoreBtn) {
         memoryElements.loadMoreBtn.classList.remove('hidden');
         memoryElements.loadMoreBtn.disabled =
            !memoryState.factsHasMore || !!memoryState.searchQuery;
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

      // Keep Load More visible but disabled (no pagination for preferences)
      if (memoryElements.loadMoreBtn) {
         memoryElements.loadMoreBtn.disabled = true;
      }

      if (memoryState.preferences.length === 0) {
         showEmptyState('No preferences stored yet');
         return;
      }

      const html = memoryState.preferences.map((pref) => renderPreferenceItem(pref)).join('');
      memoryElements.list.innerHTML = html;
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

      // Keep Load More visible but disabled (no pagination for summaries)
      if (memoryElements.loadMoreBtn) {
         memoryElements.loadMoreBtn.disabled = true;
      }

      if (memoryState.summaries.length === 0) {
         showEmptyState(
            memoryState.searchQuery ? 'No summaries found' : 'No conversation summaries yet'
         );
         return;
      }

      const html = memoryState.summaries.map((summary) => renderSummaryItem(summary)).join('');
      memoryElements.list.innerHTML = html;
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

   /**
    * Set up event delegation for delete buttons (called once in init)
    */
   function setupDeleteDelegation() {
      if (!memoryElements.list) return;
      memoryElements.list.addEventListener('click', handleListClick);
   }

   function handleListClick(e) {
      const btn = e.target.closest('.memory-item-delete');
      if (!btn) return;

      e.stopPropagation();
      const factId = btn.dataset.factId;
      const prefCategory = btn.dataset.prefCategory;
      const summaryId = btn.dataset.summaryId;

      if (factId) {
         // Find fact content for detail display
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
         // Find preference content for detail display
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
         // Find summary content for detail display
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

      // Update Load More button state immediately based on tab
      // Always visible, but disabled on non-Facts tabs (no pagination)
      if (memoryElements.loadMoreBtn) {
         memoryElements.loadMoreBtn.classList.remove('hidden');
         // Disabled on Preferences/Summaries (no pagination) or while loading Facts
         memoryElements.loadMoreBtn.disabled = true;
      }

      switch (memoryState.activeTab) {
         case 'facts':
            memoryState.factsOffset = 0;
            requestFacts(0);
            break;
         case 'preferences':
            requestPreferences();
            break;
         case 'summaries':
            requestSummaries();
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
      if (memoryState.activeTab !== 'facts') return;
      if (!memoryState.factsHasMore) return;
      if (memoryState.loading) return;

      memoryState.factsOffset += FACTS_PAGE_SIZE;
      requestFacts(memoryState.factsOffset);
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
      handleSearchResponse,
      handleDeleteFactResponse,
      handleDeletePreferenceResponse,
      handleDeleteSummaryResponse,
      handleDeleteAllResponse,
   };
})();
