/**
 * Plan Orchestrator — real-time progress display for multi-step plan execution
 *
 * Shows a purple collapsible block in the transcript with step-by-step progress.
 * Ephemeral UI — not persisted to conversation history.
 */

(function (global) {
   'use strict';

   // Current orchestrator state
   let activeBlock = null;
   let stepElements = [];

   /**
    * Format milliseconds as human-readable duration
    */
   function formatDuration(ms) {
      if (ms < 1000) return ms + 'ms';
      return (ms / 1000).toFixed(1) + 's';
   }

   /**
    * Toggle collapse state of the plan block
    */
   function toggleBlock(block) {
      if (!block) return;
      const isCollapsed = block.classList.toggle('collapsed');
      const header = block.querySelector('.plan-header');
      if (header) {
         header.setAttribute('aria-expanded', !isCollapsed);
      }
   }

   /**
    * Create and append the orchestrator block to the transcript
    */
   function createOrchestratorBlock() {
      const transcript = typeof DawnElements !== 'undefined' ? DawnElements.transcript : null;
      if (!transcript) return null;

      // Remove placeholder if present
      const placeholder = transcript.querySelector('.transcript-placeholder');
      if (placeholder) placeholder.remove();

      const block = document.createElement('div');
      block.className = 'plan-block';
      block.setAttribute('role', 'region');
      block.setAttribute('aria-label', 'Plan execution progress');

      block.innerHTML = `
      <div class="plan-header" role="button" tabindex="0" aria-expanded="true">
        <span class="plan-icon" aria-hidden="true">
          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
            <polygon points="12 2 2 7 12 12 22 7 12 2"/>
            <polyline points="2 17 12 22 22 17"/>
            <polyline points="2 12 12 17 22 12"/>
          </svg>
        </span>
        <span class="plan-label">Plan</span>
        <span class="plan-progress-text"></span>
        <span class="plan-toggle" aria-hidden="true">&#9660;</span>
      </div>
      <div class="plan-content">
        <ol class="plan-steps" aria-live="polite"></ol>
      </div>
    `;

      // Toggle on click/keyboard
      const header = block.querySelector('.plan-header');
      header.addEventListener('click', () => toggleBlock(block));
      header.addEventListener('keydown', (e) => {
         if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            toggleBlock(block);
         }
      });

      transcript.appendChild(block);
      transcript.scrollTop = transcript.scrollHeight;

      return block;
   }

   /**
    * Add a new running step row
    */
   function addStep(index, name) {
      if (!activeBlock) return;
      const list = activeBlock.querySelector('.plan-steps');
      if (!list) return;

      const li = document.createElement('li');
      li.className = 'plan-step running';
      li.setAttribute('aria-current', 'step');
      li.setAttribute('aria-label', 'Running ' + name);

      li.innerHTML = `
        <span class="plan-step-indicator" aria-hidden="true">&#9654;</span>
        <span class="plan-step-name"></span>
        <span class="plan-step-duration">
          <span class="plan-dots"><span></span><span></span><span></span></span>
        </span>
      `;
      li.querySelector('.plan-step-name').textContent = name;

      // Clear aria-current from previous steps
      stepElements.forEach((el) => el.removeAttribute('aria-current'));

      list.appendChild(li);
      stepElements[index] = li;

      // Update progress text
      updateProgressText();

      // Auto-scroll
      li.scrollIntoView({ behavior: 'smooth', block: 'nearest' });

      // Also scroll transcript
      const transcript = typeof DawnElements !== 'undefined' ? DawnElements.transcript : null;
      if (transcript) {
         transcript.scrollTop = transcript.scrollHeight;
      }
   }

   /**
    * Complete a step with checkmark and duration
    */
   function completeStep(index, durationMs, success) {
      const li = stepElements[index];
      if (!li) return;

      li.className = success ? 'plan-step complete' : 'plan-step error';
      li.removeAttribute('aria-current');
      li.setAttribute(
         'aria-label',
         li.querySelector('.plan-step-name').textContent + ' ' + (success ? 'completed' : 'failed')
      );

      const indicator = li.querySelector('.plan-step-indicator');
      const duration = li.querySelector('.plan-step-duration');

      if (success) {
         indicator.textContent = '\u2713'; // checkmark
         duration.textContent = formatDuration(durationMs);
      } else {
         indicator.textContent = '\u2717'; // X
         duration.textContent = formatDuration(durationMs);
      }

      updateProgressText();
   }

   /**
    * Mark a step as errored (for validation failures before execution)
    */
   function errorStep(index, error) {
      // Create a step entry if we don't have one for this index
      const li = stepElements[index];
      if (!li) return;

      li.className = 'plan-step error';
      li.removeAttribute('aria-current');

      const indicator = li.querySelector('.plan-step-indicator');
      const duration = li.querySelector('.plan-step-duration');

      indicator.textContent = '\u2717'; // X
      duration.textContent = error || 'failed';

      updateProgressText();
   }

   /**
    * Update the header progress text (e.g., "2/3 steps")
    */
   function updateProgressText() {
      if (!activeBlock) return;
      const progressEl = activeBlock.querySelector('.plan-progress-text');
      if (!progressEl) return;

      const completed = stepElements.filter(
         (el) => el && (el.classList.contains('complete') || el.classList.contains('error'))
      ).length;
      const total = stepElements.filter((el) => el).length;

      if (total > 0) {
         progressEl.textContent = completed + '/' + total + ' steps';
      }
   }

   /**
    * Finalize the block: mark completed, update timing, auto-collapse
    */
   function finalizePlan(totalMs, toolCalls) {
      if (!activeBlock) return;

      activeBlock.classList.add('completed');

      const progressEl = activeBlock.querySelector('.plan-progress-text');
      if (progressEl) {
         progressEl.textContent =
            toolCalls +
            ' tool' +
            (toolCalls !== 1 ? 's' : '') +
            ' \u00b7 ' +
            formatDuration(totalMs);
      }

      const label = activeBlock.querySelector('.plan-label');
      if (label) label.textContent = 'Plan completed';

      // Auto-collapse after 1.5s
      setTimeout(() => {
         if (activeBlock && !activeBlock.classList.contains('collapsed')) {
            toggleBlock(activeBlock);
         }
      }, 1500);
   }

   /**
    * Handle plan error (overall plan failure)
    */
   function finalizePlanError(error) {
      if (!activeBlock) return;

      activeBlock.classList.add('completed');

      const label = activeBlock.querySelector('.plan-label');
      if (label) {
         label.textContent = 'Plan failed';
         label.style.color = 'var(--error)';
      }

      const progressEl = activeBlock.querySelector('.plan-progress-text');
      if (progressEl) {
         progressEl.textContent = error || 'error';
      }
   }

   /**
    * Main message handler — dispatches on payload.action
    */
   function handlePlanProgress(payload) {
      if (!payload || !payload.action) return;

      switch (payload.action) {
         case 'start':
            reset();
            activeBlock = createOrchestratorBlock();
            break;

         case 'step_start':
            addStep(payload.index, payload.name || 'unknown');
            break;

         case 'step_done':
            completeStep(payload.index, payload.duration_ms || 0, payload.success !== false);
            break;

         case 'step_error':
            // step_error comes for validation failures (no step_start preceded it)
            if (!stepElements[payload.index]) {
               addStep(payload.index, payload.name || 'unknown');
            }
            errorStep(payload.index, payload.error || 'failed');
            break;

         case 'done':
            finalizePlan(payload.total_ms || 0, payload.tool_calls || 0);
            break;

         case 'error':
            finalizePlanError(payload.error);
            break;
      }
   }

   /**
    * Reset state (on conversation switch or new plan)
    */
   function reset() {
      activeBlock = null;
      stepElements = [];
   }

   // Export
   global.DawnPlanOrchestrator = {
      handlePlanProgress: handlePlanProgress,
      reset: reset,
   };
})(window);
