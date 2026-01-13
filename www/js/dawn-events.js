/**
 * DAWN Event Bus - Simple pub/sub for decoupled module communication
 *
 * Usage:
 *   DawnEvents.on('token', callback)      // Subscribe
 *   DawnEvents.emit('token', data)        // Publish
 *   DawnEvents.off('token', callback)     // Unsubscribe
 *
 * Events:
 *   - token: Token received during streaming (for hesitation tracking)
 *   - metrics: Metrics update from server
 *   - state: State change (idle, listening, thinking, speaking, error)
 *   - stream:start: LLM stream started
 *   - stream:end: LLM stream ended
 *   - config: Configuration loaded/changed
 */
(function (global) {
   'use strict';

   const listeners = new Map();

   const DawnEvents = {
      /**
       * Subscribe to an event
       * @param {string} event - Event name
       * @param {Function} callback - Handler function
       */
      on(event, callback) {
         if (!listeners.has(event)) {
            listeners.set(event, new Set());
         }
         listeners.get(event).add(callback);
      },

      /**
       * Unsubscribe from an event
       * @param {string} event - Event name
       * @param {Function} callback - Handler to remove
       */
      off(event, callback) {
         if (listeners.has(event)) {
            listeners.get(event).delete(callback);
         }
      },

      /**
       * Emit an event to all subscribers
       * @param {string} event - Event name
       * @param {*} data - Event data
       */
      emit(event, data) {
         if (listeners.has(event)) {
            listeners.get(event).forEach((callback) => {
               try {
                  callback(data);
               } catch (e) {
                  console.error(`Error in event handler for '${event}':`, e);
               }
            });
         }
      },

      /**
       * Subscribe to an event for a single invocation
       * @param {string} event - Event name
       * @param {Function} callback - Handler function (called once)
       */
      once(event, callback) {
         const wrapper = (data) => {
            this.off(event, wrapper);
            callback(data);
         };
         this.on(event, wrapper);
      },
   };

   // Expose globally
   global.DawnEvents = DawnEvents;
})(window);
