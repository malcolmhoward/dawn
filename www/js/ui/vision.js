/**
 * DAWN WebUI - Vision Image Handling Module
 * Includes: file upload, paste, drag-and-drop, client-side compression
 * Supports multiple images (up to 5) per message.
 *
 * Security: SVG explicitly excluded to prevent XSS attacks.
 * All data URIs are validated before rendering.
 */
(function (global) {
   'use strict';

   // Safe data URI prefixes (XSS prevention - SVG excluded)
   const SAFE_PREFIXES = [
      'data:image/jpeg;base64,',
      'data:image/png;base64,',
      'data:image/gif;base64,',
      'data:image/webp;base64,',
   ];

   // Allowed MIME types (SVG explicitly excluded for XSS prevention)
   const VALID_TYPES = ['image/jpeg', 'image/png', 'image/gif', 'image/webp'];

   // Thumbnail settings for conversation history storage
   // Sized for readability while staying reasonable for storage
   const THUMBNAIL_MAX_DIM = 600; // Max dimension for storage thumbnail
   const THUMBNAIL_QUALITY = 0.85; // JPEG quality for thumbnail (better readability)
   const THUMBNAIL_MAX_SIZE = 150 * 1024; // 150KB max for stored thumbnail (security limit)

   // DOM element references
   let imageBtn = null;
   let imageInput = null;
   let previewContainer = null;
   let loadingOverlay = null;
   let dropTarget = null;
   let textInput = null;
   let imageCounter = null;
   let announcer = null;

   /**
    * Initialize the vision module
    */
   function init() {
      imageBtn = document.getElementById('image-btn');
      imageInput = document.getElementById('image-input');
      previewContainer = document.getElementById('image-preview');
      loadingOverlay = document.querySelector('.preview-loading');
      dropTarget = document.getElementById('input-area');
      textInput = document.getElementById('text-input');
      imageCounter = document.getElementById('image-counter');
      announcer = document.getElementById('vision-announcer');

      if (!imageBtn || !imageInput) {
         console.warn('Vision: Required elements not found');
         return;
      }

      // Enable multiple file selection
      imageInput.setAttribute('multiple', 'multiple');

      bindEvents();
      // Vision support check will happen when config is received
   }

   /**
    * Bind event handlers
    */
   function bindEvents() {
      // File input change
      imageInput.addEventListener('change', handleFileSelect);

      // Upload button click
      imageBtn.addEventListener('click', () => imageInput.click());

      // Paste handler on text input
      if (textInput) {
         textInput.addEventListener('paste', handlePaste);
      }

      // Drag-and-drop handlers
      if (dropTarget) {
         dropTarget.addEventListener('dragenter', handleDragEnter);
         dropTarget.addEventListener('dragover', handleDragOver);
         dropTarget.addEventListener('dragleave', handleDragLeave);
         dropTarget.addEventListener('drop', handleDrop);
      }
   }

   /**
    * Check if drag event contains image files
    */
   function hasImageFile(e) {
      const items = e.dataTransfer?.items;
      if (!items) return false;
      return Array.from(items).some(
         (item) => item.kind === 'file' && item.type.startsWith('image/')
      );
   }

   /**
    * Handle drag enter event
    */
   function handleDragEnter(e) {
      e.preventDefault();
      if (hasImageFile(e) && DawnState.visionState.visionEnabled) {
         dropTarget.classList.add('drag-active');
      }
   }

   /**
    * Handle drag over event (required for drop to work)
    */
   function handleDragOver(e) {
      e.preventDefault();
   }

   /**
    * Handle drag leave event
    */
   function handleDragLeave(e) {
      e.preventDefault();
      // Only remove class if leaving the drop target entirely
      if (!dropTarget.contains(e.relatedTarget)) {
         dropTarget.classList.remove('drag-active');
      }
   }

   /**
    * Handle drop event - supports multiple files
    */
   function handleDrop(e) {
      e.preventDefault();
      dropTarget.classList.remove('drag-active');

      if (!DawnState.visionState.visionEnabled) {
         DawnToast.show('Vision not available for current model', 'warning');
         return;
      }

      const files = Array.from(e.dataTransfer?.files || []);
      const imageFiles = files.filter(
         (f) => f.type.startsWith('image/') && VALID_TYPES.includes(f.type)
      );

      if (imageFiles.length === 0) return;

      // Sort alphabetically by filename when multiple files dropped together
      imageFiles.sort((a, b) => a.name.localeCompare(b.name));

      processMultipleImages(imageFiles);
   }

   /**
    * Handle file input selection - supports multiple files
    */
   async function handleFileSelect(event) {
      const files = Array.from(event.target.files || []);
      if (files.length > 0) {
         // Sort alphabetically
         files.sort((a, b) => a.name.localeCompare(b.name));
         await processMultipleImages(files);
      }
      // Reset input so same file can be selected again
      event.target.value = '';
   }

   /**
    * Handle paste event - capture images from clipboard
    */
   async function handlePaste(event) {
      const items = event.clipboardData?.items;
      if (!items) return;

      const imageItems = [];
      for (const item of items) {
         // SECURITY: Explicit whitelist (excludes SVG)
         if (VALID_TYPES.includes(item.type)) {
            imageItems.push(item);
         }
      }

      if (imageItems.length === 0) return;

      if (!DawnState.visionState.visionEnabled) {
         DawnToast.show('Vision not available for current model', 'warning');
         return;
      }

      event.preventDefault();

      // Convert items to files and process
      const files = imageItems.map((item) => item.getAsFile()).filter(Boolean);
      await processMultipleImages(files);
   }

   /**
    * Process multiple image files
    * @param {File[]} files - Array of image files to process
    */
   async function processMultipleImages(files) {
      const { pendingImages, maxImages } = DawnState.visionState;
      const availableSlots = maxImages - pendingImages.length;

      if (availableSlots <= 0) {
         DawnToast.show(`Maximum ${maxImages} images allowed`, 'warning');
         return;
      }

      // Limit to available slots
      const filesToProcess = files.slice(0, availableSlots);
      if (files.length > availableSlots) {
         DawnToast.show(`Only adding ${availableSlots} image(s), limit is ${maxImages}`, 'warning');
      }

      setLoading(true);

      for (const file of filesToProcess) {
         await processImage(file);
      }

      setLoading(false);
      updateCounter();
   }

   /**
    * Process a single image file - validate, compress, and add to pending
    * Generates both full image (for LLM) and thumbnail (for history storage)
    */
   async function processImage(file) {
      // Validate type (SECURITY: explicit whitelist, no SVG)
      if (!VALID_TYPES.includes(file.type)) {
         DawnToast.show('Unsupported image type. Use JPEG, PNG, GIF, or WebP.', 'error');
         return;
      }

      try {
         // Compress/resize image client-side (memory efficiency)
         const compressed = await compressImage(file);

         // Validate compressed size
         if (compressed.size > DawnState.visionState.maxSize) {
            DawnToast.show(`Image "${file.name}" too large after compression (max 4MB)`, 'error');
            return;
         }

         // Read full image as base64 (for LLM)
         const dataUrl = await readAsDataURL(compressed);

         // SECURITY: Validate data URI format before storing
         if (!isSafeDataUri(dataUrl)) {
            DawnToast.show('Invalid image data', 'error');
            return;
         }

         // Generate smaller thumbnail for conversation history storage
         const thumbnailDataUrl = await generateThumbnail(compressed);

         const base64 = dataUrl.split(',')[1];
         const imageData = {
            data: base64,
            mimeType: compressed.type,
            thumbnail: thumbnailDataUrl,
            previewUrl: dataUrl, // Keep for preview display
         };

         DawnState.visionState.pendingImages.push(imageData);
         addPreviewItem(dataUrl, DawnState.visionState.pendingImages.length - 1);

         // Announce to screen readers
         const count = DawnState.visionState.pendingImages.length;
         const max = DawnState.visionState.maxImages;
         announce(`Image added. ${count} of ${max} images attached.`);
      } catch (err) {
         console.error('Image processing error:', err);
         DawnToast.show(`Failed to process image: ${file.name}`, 'error');
      }
   }

   /**
    * Compress image to max 1024px on longest edge, JPEG 85% quality
    * Optimized for LLM vision APIs (~1MP = ~1400 tokens on Claude)
    * All formats converted to JPEG for consistent small file sizes
    */
   function compressImage(file) {
      const maxDim = DawnState.visionState.maxDimension;

      return new Promise((resolve, reject) => {
         const img = new Image();

         img.onload = () => {
            let { width, height } = img;

            // Only resize if exceeds max dimension
            if (width > maxDim || height > maxDim) {
               const scale = Math.min(maxDim / width, maxDim / height);
               width = Math.round(width * scale);
               height = Math.round(height * scale);
            }

            const canvas = document.createElement('canvas');
            canvas.width = width;
            canvas.height = height;
            const ctx = canvas.getContext('2d');

            // Fill with white background (handles PNG transparency)
            ctx.fillStyle = '#FFFFFF';
            ctx.fillRect(0, 0, width, height);

            ctx.drawImage(img, 0, 0, width, height);

            // Always output JPEG 85% for consistent small file sizes
            canvas.toBlob(
               (blob) => {
                  if (blob) {
                     resolve(blob);
                  } else {
                     reject(new Error('Canvas toBlob failed'));
                  }
               },
               'image/jpeg',
               0.85
            );

            // Cleanup object URL
            URL.revokeObjectURL(img.src);
         };

         img.onerror = () => {
            URL.revokeObjectURL(img.src);
            reject(new Error('Image load failed'));
         };

         img.src = URL.createObjectURL(file);
      });
   }

   /**
    * Read a blob as a data URL
    */
   function readAsDataURL(blob) {
      return new Promise((resolve, reject) => {
         const reader = new FileReader();
         reader.onload = () => resolve(reader.result);
         reader.onerror = () => reject(reader.error);
         reader.readAsDataURL(blob);
      });
   }

   /**
    * Generate a small thumbnail for conversation history storage
    * Always JPEG for consistent compression
    *
    * @param {Blob} originalBlob - The compressed image blob
    * @returns {Promise<string|null>} - Data URL of thumbnail, or null if too large
    */
   async function generateThumbnail(originalBlob) {
      return new Promise((resolve) => {
         const img = new Image();

         img.onload = async () => {
            let { width, height } = img;

            // Scale down to thumbnail size
            if (width > THUMBNAIL_MAX_DIM || height > THUMBNAIL_MAX_DIM) {
               const scale = Math.min(THUMBNAIL_MAX_DIM / width, THUMBNAIL_MAX_DIM / height);
               width = Math.round(width * scale);
               height = Math.round(height * scale);
            }

            const canvas = document.createElement('canvas');
            canvas.width = width;
            canvas.height = height;
            const ctx = canvas.getContext('2d');

            // White background for JPEG (no transparency)
            ctx.fillStyle = '#FFFFFF';
            ctx.fillRect(0, 0, width, height);
            ctx.drawImage(img, 0, 0, width, height);

            // Convert to JPEG for consistent compression
            canvas.toBlob(
               async (blob) => {
                  URL.revokeObjectURL(img.src);

                  if (!blob) {
                     resolve(null);
                     return;
                  }

                  // SECURITY: Enforce max thumbnail size
                  if (blob.size > THUMBNAIL_MAX_SIZE) {
                     console.warn(
                        `Thumbnail too large (${blob.size} > ${THUMBNAIL_MAX_SIZE}), skipping storage`
                     );
                     resolve(null);
                     return;
                  }

                  try {
                     const dataUrl = await readAsDataURL(blob);
                     // SECURITY: Validate before returning
                     if (isSafeDataUri(dataUrl)) {
                        resolve(dataUrl);
                     } else {
                        resolve(null);
                     }
                  } catch (e) {
                     resolve(null);
                  }
               },
               'image/jpeg',
               THUMBNAIL_QUALITY
            );
         };

         img.onerror = () => {
            URL.revokeObjectURL(img.src);
            resolve(null);
         };

         img.src = URL.createObjectURL(originalBlob);
      });
   }

   /**
    * SECURITY: Validate data URI is safe for rendering
    * Prevents XSS via SVG or malformed data URIs
    */
   function isSafeDataUri(uri) {
      if (typeof uri !== 'string') return false;

      // Check against safe prefixes (case-insensitive)
      const hasValidPrefix = SAFE_PREFIXES.some((prefix) =>
         uri.toLowerCase().startsWith(prefix.toLowerCase())
      );
      if (!hasValidPrefix) return false;

      // Validate base64 portion contains only valid characters
      const base64Part = uri.split(',')[1];
      if (!base64Part) return false;

      return /^[A-Za-z0-9+/=]+$/.test(base64Part);
   }

   /**
    * Announce message to screen readers via aria-live region
    * @param {string} message - Message to announce
    */
   function announce(message) {
      if (!announcer) return;
      // Clear and re-set to ensure announcement even if same text
      announcer.textContent = '';
      // Small delay ensures screen readers pick up the change
      setTimeout(() => {
         announcer.textContent = message;
      }, 50);
   }

   /**
    * Set loading state
    */
   function setLoading(loading) {
      DawnState.visionState.isProcessing = loading;
      if (imageBtn) {
         imageBtn.classList.toggle('loading', loading);
      }
      if (loadingOverlay) {
         loadingOverlay.classList.toggle('hidden', !loading);
      }
   }

   /**
    * Add a preview item to the preview container
    * @param {string} dataUrl - Image data URL for preview
    * @param {number} index - Index in pendingImages array
    */
   function addPreviewItem(dataUrl, index) {
      if (!previewContainer) return;

      previewContainer.classList.remove('hidden');

      const item = document.createElement('div');
      item.className = 'preview-item';
      item.dataset.index = index;
      item.setAttribute('role', 'group');
      item.setAttribute('aria-label', `Attached image ${index + 1}`);

      const img = document.createElement('img');
      img.src = dataUrl;
      img.alt = `Attached image ${index + 1}`;

      const removeBtn = document.createElement('button');
      removeBtn.className = 'preview-remove';
      removeBtn.innerHTML = '&times;';
      removeBtn.title = `Remove image ${index + 1}`;
      removeBtn.setAttribute('aria-label', `Remove image ${index + 1}`);
      removeBtn.addEventListener('click', (e) => {
         e.stopPropagation();
         removeImage(index);
      });

      item.appendChild(img);
      item.appendChild(removeBtn);
      previewContainer.appendChild(item);
   }

   /**
    * Update the image counter display
    */
   function updateCounter() {
      const count = DawnState.visionState.pendingImages.length;
      const max = DawnState.visionState.maxImages;

      if (imageCounter) {
         if (count > 0) {
            imageCounter.textContent = `${count}/${max}`;
            imageCounter.classList.remove('hidden');
         } else {
            imageCounter.classList.add('hidden');
         }
      }

      // Disable button if at max
      if (imageBtn) {
         const atLimit = count >= max;
         imageBtn.classList.toggle('at-limit', atLimit);
         imageBtn.title = atLimit
            ? `Maximum ${max} images reached`
            : DawnState.visionState.visionEnabled
              ? 'Attach image'
              : 'Vision not available for current model';
      }
   }

   /**
    * Remove an image by index
    * @param {number} index - Index of image to remove
    */
   function removeImage(index) {
      const { pendingImages } = DawnState.visionState;
      if (index < 0 || index >= pendingImages.length) return;

      // Remove from array
      pendingImages.splice(index, 1);

      // Rebuild preview container
      rebuildPreviews();
      updateCounter();

      // Announce to screen readers
      const count = pendingImages.length;
      if (count === 0) {
         announce('Image removed. No images attached.');
      } else {
         const max = DawnState.visionState.maxImages;
         announce(`Image removed. ${count} of ${max} images attached.`);
      }
   }

   /**
    * Rebuild all preview items (after removal)
    */
   function rebuildPreviews() {
      if (!previewContainer) return;

      // Clear existing previews
      previewContainer.innerHTML = '';

      const { pendingImages } = DawnState.visionState;

      if (pendingImages.length === 0) {
         previewContainer.classList.add('hidden');
         return;
      }

      // Re-add all previews with updated indices
      pendingImages.forEach((img, idx) => {
         addPreviewItem(img.previewUrl, idx);
      });
   }

   /**
    * Clear all pending images
    */
   /**
    * Clear all pending images
    * @param {boolean} silent - If true, don't announce (e.g., after sending message)
    */
   function clearImages(silent = false) {
      const hadImages = DawnState.visionState.pendingImages.length > 0;
      DawnState.visionState.pendingImages = [];
      if (previewContainer) {
         previewContainer.innerHTML = '';
         previewContainer.classList.add('hidden');
      }
      if (imageInput) {
         imageInput.value = '';
      }
      updateCounter();

      // Announce only if there were images and not silent
      if (hadImages && !silent) {
         announce('All images removed.');
      }
   }

   /**
    * Update vision limits from server config
    * Called when config response is received with vision_limits
    * @param {Object} limits - Server-provided limits
    * @param {number} limits.max_images - Maximum images per message
    * @param {number} limits.max_image_size - Maximum image size in bytes
    * @param {number} limits.max_dimension - Maximum image dimension in pixels
    * @param {number} limits.max_thumbnail_size - Maximum thumbnail size in bytes
    */
   function updateLimits(limits) {
      if (!limits) return;

      const state = DawnState.visionState;

      if (typeof limits.max_images === 'number' && limits.max_images > 0) {
         state.maxImages = limits.max_images;
      }
      if (typeof limits.max_image_size === 'number' && limits.max_image_size > 0) {
         state.maxSize = limits.max_image_size;
      }
      if (typeof limits.max_dimension === 'number' && limits.max_dimension > 0) {
         state.maxDimension = limits.max_dimension;
      }

      // Update counter display with new limits
      updateCounter();

      console.log('Vision limits updated from server:', {
         maxImages: state.maxImages,
         maxSize: state.maxSize,
         maxDimension: state.maxDimension,
      });
   }

   /**
    * Check if vision is supported and update UI accordingly
    * Called when config is received or LLM model changes
    */
   function checkVisionSupport(config) {
      // Check if vision is enabled in config
      // This will be determined by the current model's capabilities
      let visionEnabled = false;

      if (config?.llm?.cloud?.vision_enabled) {
         visionEnabled = true;
      } else if (config?.llm?.local?.vision_enabled) {
         visionEnabled = true;
      }

      // Also check model name for known vision models
      const model = config?.llm?.cloud?.model || config?.llm?.local?.model || '';
      const visionModels = [
         'gpt-4o',
         'gpt-4-vision',
         'gpt-4-turbo',
         'claude-3',
         'gemini',
         'llava',
         'qwen-vl',
         'cogvlm',
      ];
      if (visionModels.some((vm) => model.toLowerCase().includes(vm))) {
         visionEnabled = true;
      }

      DawnState.visionState.visionEnabled = visionEnabled;

      if (imageBtn) {
         imageBtn.disabled = !visionEnabled;
         imageBtn.title = visionEnabled ? 'Attach image' : 'Vision not available for current model';
      }

      updateCounter();
   }

   /**
    * Get all pending images for sending (full resolution for LLM)
    * @returns {Array<{data: string, mimeType: string}>}
    */
   function getPendingImages() {
      return DawnState.visionState.pendingImages.map((img) => ({
         data: img.data,
         mimeType: img.mimeType,
      }));
   }

   /**
    * Get all thumbnail data URLs for history storage
    * @returns {string[]} Array of data URLs (nulls filtered out)
    */
   function getPendingThumbnails() {
      return DawnState.visionState.pendingImages
         .map((img) => img.thumbnail)
         .filter((t) => t !== null);
   }

   /**
    * Check if there are pending images
    */
   function hasPendingImages() {
      return DawnState.visionState.pendingImages.length > 0;
   }

   /**
    * Format text with image markers for storage (multiple images)
    * SECURITY: Only accepts validated data URIs
    *
    * @param {string} text - User's message text
    * @param {string[]} thumbnailDataUrls - Array of validated thumbnail data URLs
    * @returns {string} - Formatted message with image markers
    */
   function formatMessageWithImages(text, thumbnailDataUrls) {
      if (!thumbnailDataUrls || thumbnailDataUrls.length === 0) {
         return text;
      }

      // Filter to only safe data URIs
      const safeUrls = thumbnailDataUrls.filter((url) => isSafeDataUri(url));
      if (safeUrls.length === 0) {
         return text;
      }

      // Append each image as separate marker on new line
      let result = text;
      for (const url of safeUrls) {
         result += '\n[IMAGE:' + url + ']';
      }
      return result;
   }

   /**
    * Parse image markers from stored message (supports multiple images)
    * SECURITY: Validates data URIs before returning
    *
    * @param {string} content - Message content potentially containing image markers
    * @returns {{ text: string, imageDataUrls: string[] }} - Parsed text and images
    */
   function parseImageMarkers(content) {
      if (!content || typeof content !== 'string') {
         return { text: content || '', imageDataUrls: [] };
      }

      const imageDataUrls = [];
      let text = content;

      // Find all [IMAGE:data:image/...] markers
      const markerRegex = /\n?\[IMAGE:(data:image\/[^\[\]]+)\]/g;
      let match;

      while ((match = markerRegex.exec(content)) !== null) {
         const dataUrl = match[1];
         // SECURITY: Validate each extracted data URI
         if (isSafeDataUri(dataUrl)) {
            imageDataUrls.push(dataUrl);
         } else {
            console.warn('Invalid image data URI in stored message, ignoring');
         }
      }

      // Remove all markers from text
      text = content.replace(markerRegex, '').trimEnd();

      return { text, imageDataUrls };
   }

   // Legacy single-image functions for backward compatibility
   function getPendingImage() {
      const images = DawnState.visionState.pendingImages;
      return images.length > 0 ? images[0] : null;
   }

   function getPendingThumbnail() {
      const images = DawnState.visionState.pendingImages;
      return images.length > 0 ? images[0]?.thumbnail || null : null;
   }

   function hasPendingImage() {
      return DawnState.visionState.pendingImages.length > 0;
   }

   // Legacy wrapper for single image format
   function formatMessageWithImage(text, thumbnailDataUrl) {
      if (!thumbnailDataUrl) return text;
      return formatMessageWithImages(text, [thumbnailDataUrl]);
   }

   // Legacy wrapper for single image parse
   function parseImageMarker(content) {
      const { text, imageDataUrls } = parseImageMarkers(content);
      return {
         text,
         imageDataUrl: imageDataUrls.length > 0 ? imageDataUrls[0] : null,
      };
   }

   // Alias for backward compatibility
   const clearImage = clearImages;

   // Export module
   global.DawnVision = {
      init,
      clearImage, // Legacy alias
      clearImages,
      checkVisionSupport,
      updateLimits,
      // Multi-image API
      getPendingImages,
      getPendingThumbnails,
      hasPendingImages,
      formatMessageWithImages,
      parseImageMarkers,
      removeImage,
      // Legacy single-image API (backward compatibility)
      getPendingImage,
      getPendingThumbnail,
      hasPendingImage,
      formatMessageWithImage,
      parseImageMarker,
      // Utilities
      isSafeDataUri,
      SAFE_PREFIXES,
      THUMBNAIL_MAX_SIZE,
   };
})(window);
