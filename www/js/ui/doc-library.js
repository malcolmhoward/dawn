/**
 * DAWN Document Library Panel
 * Upload, index, list, and delete documents for RAG search.
 * Documents are chunked and embedded server-side for semantic search.
 */
(function () {
   'use strict';

   const SUPPORTED_TYPES = ['pdf', 'docx', 'txt', 'md'];
   const MAX_FILE_SIZE = 10 * 1024 * 1024; // 10 MB
   const PAGE_SIZE = 20;

   let state = {
      documents: [],
      isOpen: false,
      indexing: false,
      indexingFilename: '',
      offset: 0,
      hasMore: false,
      showAll: false,
   };

   let callbacks = {
      trapFocus: null,
      showConfirmModal: null,
   };

   // Focus management state
   let focusTrapCleanup = null;
   let triggerElement = null;

   let el = {};

   /* =============================================================================
    * Init
    * ============================================================================= */

   function init(options) {
      if (options) {
         if (options.trapFocus) callbacks.trapFocus = options.trapFocus;
         if (options.showConfirmModal) callbacks.showConfirmModal = options.showConfirmModal;
      }
      el.btn = document.getElementById('doc-library-btn');
      el.popover = document.getElementById('doc-library-popover');
      el.closeBtn = document.getElementById('doc-library-close');
      el.list = document.getElementById('doc-library-list');
      el.dropzone = document.getElementById('doc-library-dropzone');
      el.fileInput = document.getElementById('doc-library-file-input');
      el.docCount = document.getElementById('doc-library-count');
      el.chunkCount = document.getElementById('doc-library-chunks');
      el.indexingBar = document.getElementById('doc-library-indexing');
      el.loadMoreBtn = document.getElementById('doc-library-load-more');
      el.showAllLabel = document.getElementById('doc-library-show-all-label');
      el.showAllCheck = document.getElementById('doc-library-show-all');
      el.globalLabel = document.getElementById('doc-library-global-label');
      el.globalCheck = document.getElementById('doc-library-global-check');

      if (!el.btn || !el.popover) return;

      el.btn.addEventListener('click', toggle);
      el.closeBtn.addEventListener('click', close);

      // Load More button
      if (el.loadMoreBtn) {
         el.loadMoreBtn.addEventListener('click', () => {
            requestList(state.offset);
         });
      }

      // Dropzone events
      if (el.dropzone) {
         el.dropzone.addEventListener('click', () => el.fileInput?.click());
         el.dropzone.addEventListener('dragover', (e) => {
            e.preventDefault();
            el.dropzone.classList.add('dragover');
         });
         el.dropzone.addEventListener('dragleave', () => {
            el.dropzone.classList.remove('dragover');
         });
         el.dropzone.addEventListener('drop', (e) => {
            e.preventDefault();
            el.dropzone.classList.remove('dragover');
            if (e.dataTransfer?.files?.length > 0) {
               handleFileSelect(e.dataTransfer.files[0]);
            }
         });
      }

      if (el.fileInput) {
         el.fileInput.addEventListener('change', (e) => {
            if (e.target.files?.length > 0) {
               handleFileSelect(e.target.files[0]);
               e.target.value = '';
            }
         });
      }

      // Show All toggle (admin only)
      if (el.showAllCheck) {
         el.showAllCheck.addEventListener('change', () => {
            state.showAll = el.showAllCheck.checked;
            state.documents = [];
            state.offset = 0;
            state.hasMore = false;
            requestList(0);
         });
      }

      // Close on outside click
      document.addEventListener('click', (e) => {
         if (state.isOpen && !el.popover.contains(e.target) && !el.btn.contains(e.target)) {
            close();
         }
      });

      // Close on Escape
      document.addEventListener('keydown', (e) => {
         if (e.key === 'Escape' && state.isOpen) close();
      });
   }

   /* =============================================================================
    * Open / Close
    * ============================================================================= */

   function toggle() {
      if (state.isOpen) {
         close();
      } else {
         open();
      }
   }

   function open() {
      triggerElement = document.activeElement;
      el.popover.classList.remove('hidden');
      el.btn.classList.add('active');
      state.isOpen = true;

      // Set up focus trap
      if (callbacks.trapFocus) {
         focusTrapCleanup = callbacks.trapFocus(el.popover);
      }

      // Reset and fetch first page
      state.documents = [];
      state.offset = 0;
      state.hasMore = false;
      requestList(0);
   }

   function close() {
      el.popover.classList.add('hidden');
      el.btn.classList.remove('active');
      state.isOpen = false;

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
   }

   /* =============================================================================
    * WebSocket API
    * ============================================================================= */

   function requestList(offset) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      const payload = { limit: PAGE_SIZE, offset: offset || 0 };
      if (state.showAll) payload.show_all = true;
      DawnWS.send({ type: 'doc_library_list', payload });
   }

   function requestDelete(docId) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({ type: 'doc_library_delete', payload: { id: docId } });
   }

   function requestToggleGlobal(docId, isGlobal) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({
         type: 'doc_library_toggle_global',
         payload: { id: docId, is_global: isGlobal },
      });
   }

   function requestIndex(filename, filetype, text, isGlobal) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      state.indexing = true;
      state.indexingFilename = filename;
      renderIndexing();
      DawnWS.send({
         type: 'doc_library_index',
         payload: { filename, filetype, text, is_global: isGlobal || false },
      });
   }

   /* =============================================================================
    * WebSocket Response Handlers
    * ============================================================================= */

   function handleListResponse(payload) {
      if (!payload?.success) {
         console.error('doc_library list failed:', payload?.error);
         return;
      }

      const newDocs = payload.documents || [];
      state.hasMore = payload.has_more || false;

      if (state.offset === 0) {
         // First page — full render
         state.documents = newDocs;
         renderList();
      } else if (newDocs.length > 0) {
         // Subsequent page — append
         state.documents = state.documents.concat(newDocs);
         appendItems(newDocs);
      }

      state.offset = state.documents.length;
      updateLoadMore();
      updateStats();
   }

   function handleDeleteResponse(payload) {
      if (!payload?.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload?.error || 'Failed to delete document', 'error');
         }
         return;
      }
      // Remove from local state
      state.documents = state.documents.filter((d) => d.id !== payload.id);
      state.offset = state.documents.length;
      renderList();
      updateLoadMore();
      updateStats();
      if (typeof DawnToast !== 'undefined') {
         DawnToast.show('Document deleted', 'success');
      }
   }

   function handleIndexResponse(payload) {
      state.indexing = false;
      state.indexingFilename = '';
      renderIndexing();

      if (!payload?.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload?.error || 'Indexing failed', 'error');
         }
         return;
      }

      if (typeof DawnToast !== 'undefined') {
         let msg = `Indexed "${payload.filename}" (${payload.num_chunks} chunks)`;
         if (payload.failed_chunks > 0) {
            msg += ` — ${payload.failed_chunks} chunks failed`;
         }
         DawnToast.show(msg, 'success');
      }

      // Refresh list from the start
      state.documents = [];
      state.offset = 0;
      state.hasMore = false;
      requestList(0);
   }

   function handleToggleGlobalResponse(payload) {
      if (!payload?.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload?.error || 'Failed to update document', 'error');
         }
         return;
      }
      // Update local state
      const doc = state.documents.find((d) => d.id === payload.id);
      if (doc) {
         doc.is_global = payload.is_global;
         renderList();
         updateLoadMore();
      }
      const name = doc ? doc.filename : 'Document';
      if (typeof DawnToast !== 'undefined') {
         DawnToast.show(
            payload.is_global ? `"${name}" shared globally` : `"${name}" set to private`,
            'success'
         );
      }
   }

   /* =============================================================================
    * File Upload → Extract → Index
    * ============================================================================= */

   async function handleFileSelect(file) {
      const ext = file.name.split('.').pop()?.toLowerCase();
      if (!SUPPORTED_TYPES.includes(ext)) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(`Unsupported file type: .${ext}`, 'error');
         }
         return;
      }

      if (file.size > MAX_FILE_SIZE) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('File too large (max 10 MB)', 'error');
         }
         return;
      }

      // Upload to existing extraction endpoint
      const formData = new FormData();
      formData.append('document', file, file.name);

      state.indexing = true;
      state.indexingFilename = file.name;
      renderIndexing();

      try {
         const response = await fetch('/api/documents', {
            method: 'POST',
            credentials: 'include',
            body: formData,
         });

         if (!response.ok) {
            throw new Error(`Upload failed: ${response.status}`);
         }

         const result = await response.json();

         if (!result.content || result.content.length === 0) {
            throw new Error('No text could be extracted from file');
         }

         // Now index via WebSocket (chunk + embed + store)
         const isGlobal = el.globalCheck ? el.globalCheck.checked : false;
         requestIndex(result.filename || file.name, result.type || ext, result.content, isGlobal);
      } catch (err) {
         state.indexing = false;
         state.indexingFilename = '';
         renderIndexing();
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(err.message || 'Upload failed', 'error');
         }
      }
   }

   /* =============================================================================
    * Rendering
    * ============================================================================= */

   function canToggleGlobal() {
      const authState = typeof DawnState !== 'undefined' ? DawnState.authState : null;
      return authState && authState.authenticated && authState.isAdmin;
   }

   function renderDocItem(doc) {
      const date = new Date(doc.created_at * 1000).toLocaleDateString();
      const globalBadge = doc.is_global ? '<span class="global-badge">GLOBAL</span>' : '';
      const ownerBadge =
         state.showAll && doc.owner_name
            ? `<span class="owner-badge">${escapeHtml(doc.owner_name)}</span>`
            : '';
      const safeType = (doc.filetype || '').replace(/[^a-z0-9]/g, '');
      const globeSvg =
         '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><line x1="2" y1="12" x2="22" y2="12"/><path d="M12 2a15.3 15.3 0 0 1 4 10 15.3 15.3 0 0 1-4 10 15.3 15.3 0 0 1-4-10 15.3 15.3 0 0 1 4-10z"/></svg>';
      const globalToggle = canToggleGlobal()
         ? `<button type="button" class="doc-library-item-global" data-id="${doc.id}" data-global="${doc.is_global ? '1' : '0'}" title="${doc.is_global ? 'Make private' : 'Share globally'}" aria-label="${doc.is_global ? 'Make private' : 'Share globally'}">${globeSvg}</button>`
         : '';
      return `
      <div class="doc-library-item" data-id="${doc.id}">
         <div class="doc-library-item-icon ${safeType}">${escapeHtml(doc.filetype || '')}</div>
         <div class="doc-library-item-info">
            <div class="doc-library-item-name" title="${escapeHtml(doc.filename)}">${escapeHtml(doc.filename)}</div>
            <div class="doc-library-item-meta">${doc.num_chunks} chunks &middot; ${date}${globalBadge}${ownerBadge}</div>
         </div>
         ${globalToggle}
         <button type="button" class="doc-library-item-delete" data-id="${doc.id}" title="Delete">&times;</button>
      </div>`;
   }

   function handleDeleteClick(btn) {
      btn.addEventListener('click', (e) => {
         e.stopPropagation();
         const id = parseInt(btn.dataset.id, 10);
         const doc = state.documents.find((d) => d.id === id);
         const name = doc ? doc.filename : 'this document';
         if (callbacks.showConfirmModal) {
            callbacks.showConfirmModal(
               `Delete "${name}"? This will remove all indexed chunks and cannot be undone.`,
               () => requestDelete(id),
               { title: 'Delete Document', okText: 'Delete' }
            );
         } else {
            requestDelete(id);
         }
      });
   }

   function handleGlobalClick(btn) {
      btn.addEventListener('click', (e) => {
         e.stopPropagation();
         const id = parseInt(btn.dataset.id, 10);
         const currentlyGlobal = btn.dataset.global === '1';
         requestToggleGlobal(id, !currentlyGlobal);
      });
   }

   function bindItemButtons(container) {
      container.querySelectorAll('.doc-library-item-delete').forEach((btn) => {
         handleDeleteClick(btn);
      });
      container.querySelectorAll('.doc-library-item-global').forEach((btn) => {
         handleGlobalClick(btn);
      });
   }

   function renderList() {
      if (!el.list) return;

      if (state.documents.length === 0) {
         el.list.innerHTML = '<div class="doc-library-empty">No documents indexed yet</div>';
         return;
      }

      el.list.innerHTML = state.documents.map(renderDocItem).join('');
      bindItemButtons(el.list);
   }

   function appendItems(docs) {
      if (!el.list) return;

      // Clear empty message if present
      const empty = el.list.querySelector('.doc-library-empty');
      if (empty) empty.remove();

      const html = docs.map(renderDocItem).join('');
      el.list.insertAdjacentHTML('beforeend', html);

      // Bind only the newly added delete buttons
      const items = el.list.querySelectorAll('.doc-library-item');
      const newItems = Array.from(items).slice(-docs.length);
      newItems.forEach((item) => {
         const delBtn = item.querySelector('.doc-library-item-delete');
         if (delBtn) handleDeleteClick(delBtn);
         const globalBtn = item.querySelector('.doc-library-item-global');
         if (globalBtn) handleGlobalClick(globalBtn);
      });
   }

   function updateLoadMore() {
      if (!el.loadMoreBtn) return;
      if (state.hasMore) {
         el.loadMoreBtn.classList.remove('hidden');
         el.loadMoreBtn.disabled = false;
      } else {
         el.loadMoreBtn.classList.add('hidden');
      }
   }

   function renderIndexing() {
      if (!el.indexingBar) return;
      if (state.indexing) {
         el.indexingBar.classList.remove('hidden');
         el.indexingBar.innerHTML = `
            <div class="spinner"></div>
            <span>Indexing ${escapeHtml(state.indexingFilename)}...</span>`;
      } else {
         el.indexingBar.classList.add('hidden');
         el.indexingBar.innerHTML = '';
      }
   }

   function updateStats() {
      if (el.docCount) {
         el.docCount.textContent = state.documents.length;
      }
      if (el.chunkCount) {
         const total = state.documents.reduce((sum, d) => sum + d.num_chunks, 0);
         el.chunkCount.textContent = total;
      }
   }

   function escapeHtml(str) {
      const div = document.createElement('div');
      div.textContent = str;
      return div.innerHTML;
   }

   function updateVisibility() {
      if (!el.btn) return;
      const authState = typeof DawnState !== 'undefined' ? DawnState.authState : null;
      if (authState && authState.authenticated) {
         el.btn.classList.remove('hidden');
         // Show admin controls
         const isAdmin = authState.isAdmin || false;
         if (el.showAllLabel) {
            el.showAllLabel.classList.toggle('hidden', !isAdmin);
         }
         if (el.globalLabel) {
            el.globalLabel.classList.toggle('hidden', !isAdmin);
         }
      } else {
         el.btn.classList.add('hidden');
         close();
      }
   }

   /* =============================================================================
    * Public API
    * ============================================================================= */

   window.DawnDocLibrary = {
      init,
      open,
      close,
      toggle,
      updateVisibility,
      handleListResponse,
      handleDeleteResponse,
      handleIndexResponse,
      handleToggleGlobalResponse,
   };
})();
