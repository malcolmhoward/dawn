/**
 * DAWN Settings Module
 * Settings panel, configuration management, modals, and auth visibility
 */
(function () {
   'use strict';

   /* =============================================================================
    * State
    * ============================================================================= */

   let currentConfig = null;
   let currentSecrets = null;
   let restartRequiredFields = [];
   let changedFields = new Set();
   let dynamicOptions = { asr_models: [], tts_voices: [], bind_addresses: [] };

   // Settings DOM elements (populated after init)
   const settingsElements = {};

   // Audio device state
   let audioDevicesCache = { capture: [], playback: [] };

   // LLM runtime state (updated from server on connect)
   let llmRuntimeState = {
      type: 'local',
      provider: 'openai',
      model: '',
      openai_available: false,
      claude_available: false,
      gemini_available: false,
   };

   // Per-conversation LLM settings state
   let conversationLlmState = {
      tools_mode: 'native',
      thinking_mode: 'auto',
      locked: false,
   };

   // Global defaults from config (populated on config load)
   let globalDefaults = {
      type: 'cloud',
      provider: 'openai',
      openai_model: '',
      claude_model: '',
      gemini_model: '',
      tools_mode: 'native',
      thinking_mode: 'disabled', // For Claude/local: disabled/auto/enabled
      reasoning_effort: 'medium', // For OpenAI o-series/GPT-5: none/minimal/low/medium/high
   };

   // Modal state
   let confirmModalCleanup = null;
   let inputModalCleanup = null;
   let pendingConfirmCallback = null;
   let pendingInputCallback = null;
   let modalTriggerElement = null; // Track element to return focus to (M13)

   // Callbacks for external dependencies
   let callbacks = {
      getAuthState: null,
      setAuthState: null,
      updateHistoryButtonVisibility: null,
      restoreHistorySidebarState: null,
   };

   /* =============================================================================
    * Settings Schema
    * ============================================================================= */

   const SETTINGS_SCHEMA = {
      general: {
         label: 'General',
         icon: '&#x2699;',
         fields: {
            ai_name: {
               type: 'text',
               label: 'AI Name / Wake Word',
               hint: 'Wake word to activate voice input',
            },
            log_file: {
               type: 'text',
               label: 'Log File Path',
               placeholder: 'Leave empty for stdout',
               hint: 'Path to log file, or empty for console output',
            },
         },
      },
      persona: {
         label: 'Persona',
         icon: '&#x1F464;',
         fields: {
            description: {
               type: 'textarea',
               label: 'AI Description',
               rows: 3,
               placeholder: 'Custom personality description',
               hint: 'Personality and behavior instructions for the AI. Changes apply to new conversations only.',
            },
         },
      },
      localization: {
         label: 'Localization',
         icon: '&#x1F30D;',
         fields: {
            location: {
               type: 'text',
               label: 'Location',
               placeholder: 'e.g., San Francisco, CA',
               hint: 'Default location for weather and local queries',
            },
            timezone: {
               type: 'select',
               label: 'Timezone',
               hint: 'IANA timezone for time-related responses',
               options: [
                  { value: '', label: '(System default)' },
                  // Sorted by UTC offset (west to east)
                  { value: 'America/Honolulu', label: '(UTC-10:00) America/Honolulu' },
                  { value: 'America/Anchorage', label: '(UTC-09:00) America/Anchorage' },
                  { value: 'America/Los_Angeles', label: '(UTC-08:00) America/Los_Angeles' },
                  { value: 'America/Vancouver', label: '(UTC-08:00) America/Vancouver' },
                  { value: 'America/Denver', label: '(UTC-07:00) America/Denver' },
                  { value: 'America/Phoenix', label: '(UTC-07:00) America/Phoenix' },
                  { value: 'America/Chicago', label: '(UTC-06:00) America/Chicago' },
                  { value: 'America/Mexico_City', label: '(UTC-06:00) America/Mexico_City' },
                  { value: 'America/New_York', label: '(UTC-05:00) America/New_York' },
                  { value: 'America/Toronto', label: '(UTC-05:00) America/Toronto' },
                  { value: 'America/Sao_Paulo', label: '(UTC-03:00) America/Sao_Paulo' },
                  { value: 'America/Buenos_Aires', label: '(UTC-03:00) America/Buenos_Aires' },
                  { value: 'UTC', label: '(UTC+00:00) UTC' },
                  { value: 'Europe/London', label: '(UTC+00:00) Europe/London' },
                  { value: 'Europe/Paris', label: '(UTC+01:00) Europe/Paris' },
                  { value: 'Europe/Berlin', label: '(UTC+01:00) Europe/Berlin' },
                  { value: 'Europe/Rome', label: '(UTC+01:00) Europe/Rome' },
                  { value: 'Europe/Madrid', label: '(UTC+01:00) Europe/Madrid' },
                  { value: 'Europe/Amsterdam', label: '(UTC+01:00) Europe/Amsterdam' },
                  { value: 'Europe/Moscow', label: '(UTC+03:00) Europe/Moscow' },
                  { value: 'Asia/Dubai', label: '(UTC+04:00) Asia/Dubai' },
                  { value: 'Asia/Mumbai', label: '(UTC+05:30) Asia/Mumbai' },
                  { value: 'Asia/Bangkok', label: '(UTC+07:00) Asia/Bangkok' },
                  { value: 'Asia/Shanghai', label: '(UTC+08:00) Asia/Shanghai' },
                  { value: 'Asia/Hong_Kong', label: '(UTC+08:00) Asia/Hong_Kong' },
                  { value: 'Asia/Singapore', label: '(UTC+08:00) Asia/Singapore' },
                  { value: 'Australia/Perth', label: '(UTC+08:00) Australia/Perth' },
                  { value: 'Asia/Tokyo', label: '(UTC+09:00) Asia/Tokyo' },
                  { value: 'Asia/Seoul', label: '(UTC+09:00) Asia/Seoul' },
                  { value: 'Australia/Sydney', label: '(UTC+10:00) Australia/Sydney' },
                  { value: 'Australia/Melbourne', label: '(UTC+10:00) Australia/Melbourne' },
                  { value: 'Pacific/Auckland', label: '(UTC+12:00) Pacific/Auckland' },
                  { value: 'Pacific/Fiji', label: '(UTC+12:00) Pacific/Fiji' },
               ],
            },
            units: {
               type: 'select',
               label: 'Units',
               options: ['imperial', 'metric'],
               hint: 'Measurement system for weather and calculations',
            },
         },
      },
      audio: {
         label: 'Audio',
         icon: '&#x1F50A;',
         fields: {
            backend: {
               type: 'select',
               label: 'Backend',
               options: ['auto', 'pulse', 'alsa'],
               restart: true,
               hint: 'Audio system: auto detects PulseAudio or falls back to ALSA',
            },
            capture_device: {
               type: 'text',
               label: 'Capture Device',
               restart: true,
               hint: 'Microphone device name (e.g., default, hw:0,0)',
            },
            playback_device: {
               type: 'text',
               label: 'Playback Device',
               restart: true,
               hint: 'Speaker device name (e.g., default, hw:0,0)',
            },
            bargein: {
               type: 'group',
               label: 'Barge-In',
               fields: {
                  enabled: {
                     type: 'checkbox',
                     label: 'Enable Barge-In',
                     hint: 'Allow interrupting AI speech with new voice input',
                  },
                  cooldown_ms: {
                     type: 'number',
                     label: 'Cooldown (ms)',
                     min: 0,
                     hint: 'Minimum time between barge-in events',
                  },
                  startup_cooldown_ms: {
                     type: 'number',
                     label: 'Startup Cooldown (ms)',
                     min: 0,
                     hint: 'Block barge-in when TTS first starts speaking',
                  },
               },
            },
         },
      },
      commands: {
         label: 'Commands',
         icon: '&#x2328;',
         fields: {
            processing_mode: {
               type: 'select',
               label: 'Processing Mode',
               options: ['direct_only', 'llm_only', 'direct_first'],
               restart: true,
               hint: 'direct_only: pattern matching only, llm_only: AI interprets all commands, direct_first: try patterns then AI',
            },
         },
      },
      vad: {
         label: 'Voice Activity Detection',
         icon: '&#x1F3A4;',
         fields: {
            speech_threshold: {
               type: 'number',
               label: 'Speech Threshold',
               min: 0,
               max: 1,
               step: 0.05,
               hint: 'VAD confidence to start listening (higher = less sensitive)',
            },
            speech_threshold_tts: {
               type: 'number',
               label: 'Speech Threshold (TTS)',
               min: 0,
               max: 1,
               step: 0.05,
               hint: 'VAD threshold during TTS playback (higher to ignore echo)',
            },
            silence_threshold: {
               type: 'number',
               label: 'Silence Threshold',
               min: 0,
               max: 1,
               step: 0.05,
               hint: 'VAD confidence to detect end of speech',
            },
            end_of_speech_duration: {
               type: 'number',
               label: 'End of Speech (sec)',
               min: 0,
               step: 0.1,
               hint: 'Silence duration before processing speech',
            },
            max_recording_duration: {
               type: 'number',
               label: 'Max Recording (sec)',
               min: 1,
               step: 1,
               hint: 'Maximum single utterance length',
            },
            preroll_ms: {
               type: 'number',
               label: 'Preroll (ms)',
               min: 0,
               hint: 'Audio captured before VAD trigger (catches word beginnings)',
            },
         },
      },
      asr: {
         label: 'Speech Recognition',
         icon: '&#x1F4DD;',
         fields: {
            models_path: {
               type: 'text',
               label: 'Models Path',
               restart: true,
               hint: 'Directory containing Whisper model files',
            },
            model: {
               type: 'dynamic_select',
               label: 'Model',
               restart: true,
               hint: 'Whisper model size',
               dynamicKey: 'asr_models',
               allowCustom: true,
            },
         },
      },
      tts: {
         label: 'Text-to-Speech',
         icon: '&#x1F5E3;',
         fields: {
            models_path: {
               type: 'text',
               label: 'Models Path',
               restart: true,
               hint: 'Directory containing Piper voice model files',
            },
            voice_model: {
               type: 'dynamic_select',
               label: 'Voice Model',
               restart: true,
               hint: 'Piper voice model',
               dynamicKey: 'tts_voices',
               allowCustom: true,
            },
            length_scale: {
               type: 'number',
               label: 'Speed (0.5-2.0)',
               min: 0.5,
               max: 2.0,
               step: 0.05,
               hint: 'Speaking rate: <1.0 = faster, >1.0 = slower',
            },
         },
      },
      llm: {
         label: 'Language Model',
         icon: '&#x1F916;',
         fields: {
            type: {
               type: 'select',
               label: 'Default Mode',
               options: ['cloud', 'local'],
               hint: 'Server default: cloud uses APIs, local uses llama-server. Users can override per-session.',
            },
            max_tokens: {
               type: 'number',
               label: 'Max Tokens',
               min: 100,
               hint: 'Maximum tokens in LLM response',
            },
            cloud: {
               type: 'group',
               label: 'Cloud Settings',
               fields: {
                  provider: {
                     type: 'select',
                     label: 'Provider',
                     options: ['openai', 'claude', 'gemini'],
                     hint: 'Cloud LLM provider',
                  },
                  endpoint: {
                     type: 'text',
                     label: 'Custom Endpoint',
                     placeholder: 'Leave empty for default',
                     hint: 'Override API endpoint (for proxies or compatible APIs)',
                  },
                  vision_enabled: {
                     type: 'checkbox',
                     label: 'Enable Vision',
                     hint: 'Allow image analysis with vision-capable models',
                  },
                  openai_models: {
                     type: 'model_list',
                     label: 'OpenAI Models',
                     rows: 8, // LLM_CLOUD_MAX_MODELS
                     placeholder: 'gpt-4o\ngpt-4-turbo\ngpt-4o-mini',
                     hint: 'Available models for quick controls (one per line)',
                  },
                  openai_default_model_idx: {
                     type: 'model_default_select',
                     label: 'Default OpenAI Model',
                     sourceKey: 'llm.cloud.openai_models',
                     hint: 'Default model for new conversations',
                  },
                  claude_models: {
                     type: 'model_list',
                     label: 'Claude Models',
                     rows: 8, // LLM_CLOUD_MAX_MODELS
                     placeholder: 'claude-sonnet-4-20250514\nclaude-opus-4-20250514',
                     hint: 'Available models for quick controls (one per line)',
                  },
                  claude_default_model_idx: {
                     type: 'model_default_select',
                     label: 'Default Claude Model',
                     sourceKey: 'llm.cloud.claude_models',
                     hint: 'Default model for new conversations',
                  },
                  gemini_models: {
                     type: 'model_list',
                     label: 'Gemini Models',
                     rows: 8, // LLM_CLOUD_MAX_MODELS
                     placeholder: 'gemini-2.5-flash\ngemini-2.5-pro\ngemini-3-flash-preview',
                     hint: 'Available models for quick controls (one per line). 2.5+ and 3.x support reasoning.',
                  },
                  gemini_default_model_idx: {
                     type: 'model_default_select',
                     label: 'Default Gemini Model',
                     sourceKey: 'llm.cloud.gemini_models',
                     hint: 'Default model for new conversations',
                  },
               },
            },
            local: {
               type: 'group',
               label: 'Local Settings',
               fields: {
                  endpoint: {
                     type: 'text',
                     label: 'Endpoint',
                     hint: 'llama-server URL (e.g., http://127.0.0.1:8080)',
                  },
                  model: {
                     type: 'text',
                     label: 'Model',
                     placeholder: 'Leave empty for server default',
                     hint: 'Model name if server hosts multiple models',
                  },
                  vision_enabled: {
                     type: 'checkbox',
                     label: 'Enable Vision',
                     hint: 'Enable for multimodal models like LLaVA',
                  },
               },
            },
            thinking: {
               type: 'group',
               label: 'Extended Thinking',
               fields: {
                  mode: {
                     type: 'select',
                     label: 'Mode',
                     options: ['disabled', 'auto', 'enabled'],
                     hint: 'disabled: never request thinking, auto: enable for supported models, enabled: always request',
                  },
                  budget_tokens: {
                     type: 'number',
                     label: 'Budget Tokens',
                     min: 1024,
                     max: 100000,
                     step: 1000,
                     hint: 'Maximum tokens for reasoning (min 1024 for Claude)',
                  },
                  reasoning_effort: {
                     type: 'select',
                     label: 'Reasoning Effort',
                     options: ['none', 'minimal', 'low', 'medium', 'high'],
                     hint: 'OpenAI reasoning models: none (GPT-5.2), minimal (GPT-5), low/medium/high (all)',
                  },
               },
            },
         },
      },
      tool_calling: {
         label: 'Tool Calling',
         icon: '&#x1F527;',
         adminOnly: true,
         fields: {
            mode: {
               type: 'select',
               label: 'Mode',
               options: [
                  { value: 'native', label: 'Native Tools' },
                  { value: 'command_tags', label: 'Command Tags (Legacy)' },
                  { value: 'disabled', label: 'Disabled' },
               ],
               hint: 'Native Tools: LLM function calling, Command Tags: XML-style <command> tags, Disabled: no tool use',
            },
         },
         customContent: 'tools_list', // Special marker for injecting tools list
      },
      search: {
         label: 'Web Search',
         icon: '&#x1F50D;',
         fields: {
            engine: {
               type: 'select',
               label: 'Engine',
               options: ['searxng', 'disabled'],
               hint: 'Search engine for web queries (SearXNG is privacy-focused)',
            },
            endpoint: {
               type: 'text',
               label: 'Endpoint',
               hint: 'SearXNG instance URL (e.g., http://localhost:8888)',
            },
            summarizer: {
               type: 'group',
               label: 'Result Summarizer',
               fields: {
                  backend: {
                     type: 'select',
                     label: 'Backend',
                     options: ['disabled', 'local', 'default'],
                     hint: 'disabled: no summarization, local: use local LLM, default: use active LLM',
                  },
                  threshold_bytes: {
                     type: 'number',
                     label: 'Threshold (bytes)',
                     min: 0,
                     step: 512,
                     hint: 'Summarize results larger than this (0 = always summarize)',
                  },
                  target_words: {
                     type: 'number',
                     label: 'Target Words',
                     min: 50,
                     step: 50,
                     hint: 'Target word count for summarized output',
                  },
               },
            },
            title_filters: {
               type: 'string_list',
               label: 'Title Filters',
               rows: 8, // SEARCH_MAX_TITLE_FILTERS=16, but 8 rows is reasonable visible height
               placeholder: 'wordle\nconnections hints\nnyt connections',
               hint: 'Exclude results containing these terms in title (one per line, case-insensitive)',
            },
         },
      },
      url_fetcher: {
         label: 'URL Fetcher',
         icon: '&#x1F310;',
         fields: {
            flaresolverr: {
               type: 'group',
               label: 'FlareSolverr',
               fields: {
                  enabled: {
                     type: 'checkbox',
                     label: 'Enable FlareSolverr',
                     hint: 'Auto-fallback for sites with Cloudflare protection (requires FlareSolverr service)',
                  },
                  endpoint: {
                     type: 'text',
                     label: 'Endpoint',
                     hint: 'FlareSolverr API URL (e.g., http://localhost:8191/v1)',
                  },
                  timeout_sec: {
                     type: 'number',
                     label: 'Timeout (sec)',
                     min: 1,
                     max: 120,
                     hint: 'Request timeout for FlareSolverr',
                  },
                  max_response_bytes: {
                     type: 'number',
                     label: 'Max Response (bytes)',
                     min: 1024,
                     step: 1024,
                     hint: 'Maximum response size to accept',
                  },
               },
            },
         },
      },
      mqtt: {
         label: 'MQTT',
         icon: '&#x1F4E1;',
         fields: {
            enabled: {
               type: 'checkbox',
               label: 'Enable MQTT',
               hint: 'Connect to MQTT broker for smart home control',
            },
            broker: {
               type: 'text',
               label: 'Broker Address',
               hint: 'MQTT broker hostname or IP address',
            },
            port: {
               type: 'number',
               label: 'Port',
               min: 1,
               max: 65535,
               hint: 'MQTT broker port (default: 1883)',
            },
         },
      },
      network: {
         label: 'Network (DAP)',
         description: 'Dawn Audio Protocol server for ESP32 and other remote voice clients',
         icon: '&#x1F4F6;',
         fields: {
            enabled: {
               type: 'checkbox',
               label: 'Enable DAP Server',
               restart: true,
               hint: 'Accept connections from remote voice clients',
            },
            host: {
               type: 'dynamic_select',
               label: 'Bind Address',
               restart: true,
               hint: 'Network interface to listen on',
               dynamicKey: 'bind_addresses',
            },
            port: {
               type: 'number',
               label: 'Port',
               min: 1,
               max: 65535,
               restart: true,
               hint: 'TCP port for DAP connections (default: 5000)',
            },
            workers: {
               type: 'number',
               label: 'Workers',
               min: 1,
               max: 8,
               restart: true,
               hint: 'Concurrent client processing threads',
            },
         },
      },
      webui: {
         label: 'WebUI',
         icon: '&#x1F310;',
         fields: {
            enabled: {
               type: 'checkbox',
               label: 'Enable WebUI',
               hint: 'Browser-based interface for voice interaction',
            },
            port: {
               type: 'number',
               label: 'Port',
               min: 1,
               max: 65535,
               restart: true,
               hint: 'HTTP/WebSocket port (default: 3000)',
            },
            max_clients: {
               type: 'number',
               label: 'Max Clients',
               min: 1,
               restart: true,
               hint: 'Maximum concurrent browser connections',
            },
            workers: {
               type: 'number',
               label: 'ASR Workers',
               min: 1,
               max: 8,
               restart: true,
               hint: 'Parallel speech recognition threads',
            },
            bind_address: {
               type: 'dynamic_select',
               label: 'Bind Address',
               restart: true,
               hint: 'Network interface to listen on',
               dynamicKey: 'bind_addresses',
            },
            https: {
               type: 'checkbox',
               label: 'Enable HTTPS',
               restart: true,
               hint: 'Required for microphone access on remote connections',
            },
         },
      },
      shutdown: {
         label: 'Shutdown',
         icon: '&#x1F512;',
         fields: {
            enabled: {
               type: 'checkbox',
               label: 'Enable Voice Shutdown',
               hint: 'Allow system shutdown via voice command (disabled by default for security)',
            },
            passphrase: {
               type: 'text',
               label: 'Passphrase',
               hint: 'Secret phrase required to authorize shutdown (leave empty for no passphrase)',
            },
         },
      },
      debug: {
         label: 'Debug',
         icon: '&#x1F41B;',
         fields: {
            mic_record: { type: 'checkbox', label: 'Record Microphone' },
            asr_record: { type: 'checkbox', label: 'Record ASR Input' },
            aec_record: { type: 'checkbox', label: 'Record AEC' },
            record_path: { type: 'text', label: 'Recording Path' },
         },
      },
      tui: {
         label: 'Terminal UI',
         icon: '&#x1F5A5;',
         fields: {
            enabled: {
               type: 'checkbox',
               label: 'Enable TUI',
               restart: true,
               hint: 'Show terminal dashboard with real-time metrics',
            },
         },
      },
      paths: {
         label: 'Paths',
         icon: '&#x1F4C1;',
         fields: {
            music_dir: {
               type: 'text',
               label: 'Music Directory',
               hint: 'Path to music library for playback commands',
            },
            commands_config: {
               type: 'text',
               label: 'Commands Config',
               restart: true,
               hint: 'Path to device/command mappings JSON file',
            },
         },
      },
   };

   /* =============================================================================
    * DOM Element Initialization
    * ============================================================================= */

   function initElements() {
      settingsElements.panel = document.getElementById('settings-panel');
      settingsElements.overlay = document.getElementById('settings-overlay');
      settingsElements.closeBtn = document.getElementById('settings-close');
      settingsElements.openBtn = document.getElementById('settings-btn');
      settingsElements.configPath = document.getElementById('config-path-display');
      settingsElements.secretsPath = document.getElementById('secrets-path-display');
      settingsElements.sectionsContainer = document.getElementById('settings-sections');
      settingsElements.saveConfigBtn = document.getElementById('save-config-btn');
      settingsElements.saveSecretsBtn = document.getElementById('save-secrets-btn');
      settingsElements.resetBtn = document.getElementById('reset-config-btn');
      settingsElements.restartNotice = document.getElementById('restart-notice');

      // Secret inputs
      settingsElements.secretOpenai = document.getElementById('secret-openai');
      settingsElements.secretClaude = document.getElementById('secret-claude');
      settingsElements.secretGemini = document.getElementById('secret-gemini');
      settingsElements.secretMqttUser = document.getElementById('secret-mqtt-user');
      settingsElements.secretMqttPass = document.getElementById('secret-mqtt-pass');

      // Secret status indicators
      settingsElements.statusOpenai = document.getElementById('status-openai');
      settingsElements.statusClaude = document.getElementById('status-claude');
      settingsElements.statusGemini = document.getElementById('status-gemini');
      settingsElements.statusMqttUser = document.getElementById('status-mqtt-user');
      settingsElements.statusMqttPass = document.getElementById('status-mqtt-pass');

      // SmartThings elements
      settingsElements.stStatusIndicator = document.getElementById('st-status-indicator');
      settingsElements.stStatusText =
         settingsElements.stStatusIndicator?.querySelector('.st-status-text');
      settingsElements.stDevicesCountRow = document.getElementById('st-devices-count-row');
      settingsElements.stDevicesCount = document.getElementById('st-devices-count');
      settingsElements.stConnectBtn = document.getElementById('st-connect-btn');
      settingsElements.stRefreshBtn = document.getElementById('st-refresh-btn');
      settingsElements.stDisconnectBtn = document.getElementById('st-disconnect-btn');
      settingsElements.stDevicesList = document.getElementById('st-devices-list');
      settingsElements.stDevicesContainer = document.getElementById('st-devices-container');
      settingsElements.stNotConfigured = document.getElementById('st-not-configured');
   }

   /* =============================================================================
    * Panel Open/Close
    * ============================================================================= */

   function open() {
      if (!settingsElements.panel) return;

      settingsElements.panel.classList.remove('hidden');
      settingsElements.overlay.classList.remove('hidden');

      // Request config, models, and interfaces from server
      requestConfig();
      requestModelsList();
      requestInterfacesList();
      if (typeof DawnSmartThings !== 'undefined') {
         DawnSmartThings.requestStatus();
      }
   }

   function close() {
      if (!settingsElements.panel) return;

      settingsElements.panel.classList.add('hidden');
      settingsElements.overlay.classList.add('hidden');
   }

   /**
    * Open settings panel and expand a specific section
    * @param {string} sectionId - The ID of the section to expand (e.g., 'my-settings-section')
    */
   function openSection(sectionId) {
      // Open the settings panel first
      open();

      // Find and expand the target section
      const targetSection = document.getElementById(sectionId);
      if (targetSection) {
         // Expand it (remove collapsed)
         targetSection.classList.remove('collapsed');

         // Scroll section into view after a brief delay for panel to open
         setTimeout(function () {
            targetSection.scrollIntoView({ behavior: 'smooth', block: 'start' });
         }, 100);

         // Trigger section-specific data loading
         triggerSectionLoad(sectionId);
      }
   }

   /**
    * Trigger data loading for sections that need it when opened programmatically
    * @param {string} sectionId - The ID of the section being opened
    */
   function triggerSectionLoad(sectionId) {
      // Allow time for WebSocket to be ready
      setTimeout(function () {
         switch (sectionId) {
            case 'my-sessions-section':
               if (typeof DawnMySessions !== 'undefined' && DawnMySessions.requestList) {
                  DawnMySessions.requestList();
               }
               break;
            case 'user-management-section':
               if (typeof DawnUserManagement !== 'undefined' && DawnUserManagement.requestList) {
                  DawnUserManagement.requestList();
               }
               break;
            // Add other sections that need data loading here
         }
      }, 150);
   }

   /* =============================================================================
    * Config Requests
    * ============================================================================= */

   function requestConfig() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) {
         console.error('WebSocket not connected');
         return;
      }

      DawnWS.send({ type: 'get_config' });
   }

   function requestModelsList() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) {
         return;
      }

      DawnWS.send({ type: 'list_models' });
   }

   function requestInterfacesList() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) {
         return;
      }

      DawnWS.send({ type: 'list_interfaces' });
   }

   /* =============================================================================
    * Config Response Handlers
    * ============================================================================= */

   function handleModelsListResponse(payload) {
      if (payload.asr_models) {
         dynamicOptions.asr_models = payload.asr_models;
      }
      if (payload.tts_voices) {
         dynamicOptions.tts_voices = payload.tts_voices;
      }
      // Update any already-rendered dynamic selects
      updateDynamicSelects();
   }

   function handleInterfacesListResponse(payload) {
      if (payload.addresses) {
         dynamicOptions.bind_addresses = payload.addresses;
      }

      // Update any already-rendered dynamic selects
      updateDynamicSelects();
   }

   function updateDynamicSelects() {
      document.querySelectorAll('select[data-dynamic-key]').forEach((select) => {
         const key = select.dataset.dynamicKey;
         const options = dynamicOptions[key] || [];
         const currentValue = select.value;

         // Clear existing options except the first (current value placeholder)
         while (select.options.length > 1) {
            select.remove(1);
         }

         // Add options from server (skip current value to avoid duplicate)
         options.forEach((opt) => {
            if (opt === currentValue) return; // Skip - already in first option
            const option = document.createElement('option');
            option.value = opt;
            option.textContent = opt;
            select.appendChild(option);
         });

         // If current value isn't in options, mark it as "(current)"
         if (currentValue && !options.includes(currentValue)) {
            select.options[0].textContent = currentValue + ' (current)';
         }
      });
   }

   function handleGetConfigResponse(payload) {
      currentConfig = payload.config;
      currentSecrets = payload.secrets;
      restartRequiredFields = payload.requires_restart || [];
      changedFields.clear();

      // Update path displays
      if (settingsElements.configPath) {
         settingsElements.configPath.textContent = payload.config_path || 'Unknown';
      }
      if (settingsElements.secretsPath) {
         settingsElements.secretsPath.textContent = payload.secrets_path || 'Unknown';
      }

      // Update logo from AI name (capitalize for display)
      const logoEl = document.getElementById('logo');
      if (logoEl && currentConfig.general && currentConfig.general.ai_name) {
         logoEl.textContent = currentConfig.general.ai_name.toUpperCase();
      }

      // Render settings sections
      renderSettingsSections();

      // Update secrets status
      updateSecretsStatus(currentSecrets);

      // Hide restart notice initially
      if (settingsElements.restartNotice) {
         settingsElements.restartNotice.classList.add('hidden');
      }

      // Initialize audio backend state (grey out or request devices)
      const backendSelect = document.getElementById('setting-audio-backend');
      if (backendSelect) {
         // Add backend change listener
         backendSelect.addEventListener('change', () => {
            updateAudioBackendState(backendSelect.value);
         });

         // Initialize state based on current value
         updateAudioBackendState(backendSelect.value);
      }

      // Extract cloud model lists from config for quick controls dropdown
      updateCloudModelLists(currentConfig);

      // Extract global defaults from config for new conversation reset
      extractGlobalDefaults(currentConfig);

      // Apply defaults to conversation controls (reasoning, tools dropdowns)
      applyGlobalDefaultsToControls();

      // Update LLM runtime controls
      if (payload.llm_runtime) {
         updateLlmControls(payload.llm_runtime);
      }

      // Update auth state and UI visibility
      if (callbacks.setAuthState) {
         callbacks.setAuthState({
            authenticated: payload.authenticated || false,
            isAdmin: payload.is_admin || false,
            username: payload.username || '',
         });
      }
      updateAuthVisibility();
   }

   /* =============================================================================
    * Auth Visibility
    * ============================================================================= */

   function updateAuthVisibility() {
      const authState = callbacks.getAuthState ? callbacks.getAuthState() : {};

      // Toggle auth classes on body - CSS handles visibility of .admin-only elements
      document.body.classList.toggle('user-is-admin', authState.isAdmin || false);
      document.body.classList.toggle('user-authenticated', authState.authenticated || false);

      // Update user badge in header
      updateUserBadge();

      // Update history button visibility
      if (callbacks.updateHistoryButtonVisibility) {
         callbacks.updateHistoryButtonVisibility();
      }

      // Restore history sidebar state on desktop (only when authenticated)
      if (callbacks.restoreHistorySidebarState) {
         callbacks.restoreHistorySidebarState();
      }
   }

   function updateUserBadge() {
      const badgeContainer = document.getElementById('user-badge-container');
      const nameEl = document.getElementById('user-badge-name');
      const roleEl = document.getElementById('user-badge-role');

      if (!badgeContainer || !nameEl || !roleEl) return;

      const authState = callbacks.getAuthState ? callbacks.getAuthState() : {};

      if (authState.authenticated && authState.username) {
         nameEl.textContent = authState.username;
         roleEl.textContent = authState.isAdmin ? 'Admin' : 'User';
         roleEl.className = 'user-badge-role ' + (authState.isAdmin ? 'admin' : 'user');
         badgeContainer.classList.remove('hidden');
      } else {
         badgeContainer.classList.add('hidden');
      }
   }

   /* =============================================================================
    * Settings Rendering
    * ============================================================================= */

   function renderSettingsSections() {
      if (!settingsElements.sectionsContainer || !currentConfig) return;

      settingsElements.sectionsContainer.innerHTML = '';

      // Create virtual tool_calling config from llm.tools.mode
      // The tool_calling section is a UI convenience - backend uses llm.tools.mode
      const virtualConfig = { ...currentConfig };
      if (currentConfig.llm && currentConfig.llm.tools && currentConfig.llm.tools.mode) {
         virtualConfig.tool_calling = {
            mode: currentConfig.llm.tools.mode,
         };
      } else {
         virtualConfig.tool_calling = { mode: 'native' }; // Default
      }

      for (const [sectionKey, sectionDef] of Object.entries(SETTINGS_SCHEMA)) {
         const configSection = virtualConfig[sectionKey] || {};
         const sectionEl = createSettingsSection(sectionKey, sectionDef, configSection);
         settingsElements.sectionsContainer.appendChild(sectionEl);
      }
   }

   function createSettingsSection(key, def, configData) {
      const section = document.createElement('div');
      section.className = 'settings-section';
      section.dataset.section = key;

      // Add admin-only class if applicable
      if (def.adminOnly) {
         section.classList.add('admin-only');
      }

      // Header
      const header = document.createElement('h3');
      header.className = 'section-header';
      header.setAttribute('tabindex', '0'); // Make focusable for keyboard nav
      header.innerHTML = `
      <span class="section-icon">${def.icon || ''}</span>
      ${escapeHtml(def.label || '')}
      <span class="section-toggle">&#9660;</span>
    `;
      const toggleSection = () => {
         header.classList.toggle('collapsed');
         content.classList.toggle('collapsed');
         // Update aria-expanded for accessibility (M14)
         const isExpanded = !header.classList.contains('collapsed');
         header.setAttribute('aria-expanded', isExpanded);
      };
      header.addEventListener('click', toggleSection);
      header.addEventListener('keydown', (e) => {
         if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            toggleSection();
         }
      });
      header.setAttribute('aria-expanded', 'true'); // Expanded by default

      // Content
      const content = document.createElement('div');
      content.className = 'section-content';

      // Add section description if present
      if (def.description) {
         const desc = document.createElement('p');
         desc.className = 'section-description';
         desc.textContent = def.description;
         content.appendChild(desc);
      }

      // Render fields
      for (const [fieldKey, fieldDef] of Object.entries(def.fields)) {
         const value = configData[fieldKey];
         const fieldEl = createSettingField(key, fieldKey, fieldDef, value);
         content.appendChild(fieldEl);
      }

      // Inject custom content if specified
      if (def.customContent === 'tools_list') {
         content.appendChild(createToolsListContent());
      }

      section.appendChild(header);
      section.appendChild(content);

      return section;
   }

   /**
    * Create the tools list content (previously hardcoded in HTML)
    */
   function createToolsListContent() {
      const container = document.createElement('div');
      container.className = 'tools-container';
      container.innerHTML = `
      <div class="tools-info">
        <p>Configure which tools are available for LLM function calling.</p>
        <div class="tools-token-estimate">
          <span>Estimated tokens:</span>
          <span id="tools-tokens-local">Local: --</span>
          <span id="tools-tokens-remote">Remote: --</span>
        </div>
      </div>
      <div class="tools-header-row">
        <span class="tools-header-name">Tool</span>
        <span class="tools-header-local">Local</span>
        <span class="tools-header-remote">Remote</span>
      </div>
      <div id="tools-list" class="tools-list">
        <div class="tools-loading">Loading tools...</div>
      </div>
      <button id="save-tools-btn" class="save-btn">Save Tool Settings</button>
    `;

      // Re-initialize tools module with new button after DOM insertion
      setTimeout(() => {
         if (typeof DawnTools !== 'undefined') {
            DawnTools.reinitButton();
            DawnTools.requestConfig();
         }
      }, 0);

      return container;
   }

   function createSettingField(sectionKey, fieldKey, def, value) {
      const fullKey = `${sectionKey}.${fieldKey}`;

      // Handle nested groups
      if (def.type === 'group') {
         const groupEl = document.createElement('div');
         groupEl.className = 'setting-group';
         groupEl.innerHTML = `<div class="group-label">${escapeHtml(def.label || '')}</div>`;

         for (const [subKey, subDef] of Object.entries(def.fields)) {
            const subValue = value ? value[subKey] : undefined;
            const fieldEl = createSettingField(
               `${sectionKey}.${fieldKey}`,
               subKey,
               subDef,
               subValue
            );
            groupEl.appendChild(fieldEl);
         }

         return groupEl;
      }

      const item = document.createElement('div');
      item.className = def.type === 'checkbox' ? 'setting-item setting-item-row' : 'setting-item';

      // Add tooltip hint if defined
      if (def.hint) {
         item.title = def.hint;
      }

      const needsRestart = def.restart || restartRequiredFields.includes(fullKey);
      const restartBadge = needsRestart ? '<span class="restart-badge">restart</span>' : '';

      let inputHtml = '';
      const inputId = `setting-${sectionKey}-${fieldKey}`.replace(/\./g, '-');

      switch (def.type) {
         case 'text':
            inputHtml = `<input type="text" id="${inputId}" value="${escapeAttr(value || '')}" placeholder="${def.placeholder || ''}" data-key="${fullKey}">`;
            break;
         case 'number':
            const numAttrs = [
               def.min !== undefined ? `min="${def.min}"` : '',
               def.max !== undefined ? `max="${def.max}"` : '',
               def.step !== undefined ? `step="${def.step}"` : '',
            ]
               .filter(Boolean)
               .join(' ');
            inputHtml = `<input type="number" id="${inputId}" value="${formatNumber(value)}" ${numAttrs} data-key="${fullKey}">`;
            break;
         case 'checkbox':
            inputHtml = `<input type="checkbox" id="${inputId}" ${value ? 'checked' : ''} data-key="${fullKey}">`;
            break;
         case 'select':
            const options = def.options
               .map((opt) => {
                  // Support both string options and {value, label} objects
                  const optValue = typeof opt === 'object' ? opt.value : opt;
                  const optLabel =
                     typeof opt === 'object' ? opt.label : opt === '' ? '(System default)' : opt;
                  return `<option value="${optValue}" ${value === optValue ? 'selected' : ''}>${optLabel}</option>`;
               })
               .join('');
            inputHtml = `<select id="${inputId}" data-key="${fullKey}">${options}</select>`;
            break;
         case 'dynamic_select':
            // Dynamic select that gets options from server (e.g., model lists)
            const dynKey = def.dynamicKey;
            const dynOptions = dynamicOptions[dynKey] || [];
            const currentVal = value || '';

            // Start with current value as first option
            let dynOptionsHtml = `<option value="${escapeAttr(currentVal)}" selected>${escapeHtml(currentVal) || '(none)'}</option>`;

            // Add options from server if available
            dynOptions.forEach((opt) => {
               if (opt !== currentVal) {
                  dynOptionsHtml += `<option value="${escapeAttr(opt)}">${escapeHtml(opt)}</option>`;
               }
            });

            inputHtml = `<select id="${inputId}" data-key="${fullKey}" data-dynamic-key="${dynKey}">${dynOptionsHtml}</select>`;
            break;
         case 'textarea':
            inputHtml = `<textarea id="${inputId}" rows="${def.rows || 3}" placeholder="${def.placeholder || ''}" data-key="${fullKey}">${escapeHtml(value || '')}</textarea>`;
            break;
         case 'model_list':
            // Convert array to newline-separated string for editing
            const listValue = Array.isArray(value) ? value.join('\n') : value || '';
            inputHtml = `<textarea id="${inputId}" rows="${def.rows || 5}" placeholder="${def.placeholder || ''}" data-key="${fullKey}" data-type="model_list">${escapeHtml(listValue)}</textarea>`;
            break;
         case 'string_list':
            // Generic string array - same as model_list but without dropdown update behavior
            const strListValue = Array.isArray(value) ? value.join('\n') : value || '';
            inputHtml = `<textarea id="${inputId}" rows="${def.rows || 4}" placeholder="${def.placeholder || ''}" data-key="${fullKey}" data-type="string_list">${escapeHtml(strListValue)}</textarea>`;
            break;
         case 'model_default_select':
            // Dropdown populated from a model_list textarea
            // Get current options from the source model list
            const sourceKey = def.sourceKey;
            const sourceModels = getNestedValue(currentConfig, sourceKey) || [];
            const selectedIdx = parseInt(value, 10) || 0;
            let defaultSelectHtml = sourceModels
               .map(
                  (model, idx) =>
                     `<option value="${idx}" ${idx === selectedIdx ? 'selected' : ''}>${escapeHtml(model)}</option>`
               )
               .join('');
            if (sourceModels.length === 0) {
               defaultSelectHtml = '<option value="0">No models configured</option>';
            }
            inputHtml = `<select id="${inputId}" data-key="${fullKey}" data-type="model_default_select" data-source-key="${sourceKey}">${defaultSelectHtml}</select>`;
            break;
      }

      if (def.type === 'checkbox') {
         item.innerHTML = `
        ${inputHtml}
        <label for="${inputId}">${def.label} ${restartBadge}</label>
      `;
      } else {
         item.innerHTML = `
        <label for="${inputId}">${def.label} ${restartBadge}</label>
        ${inputHtml}
      `;
      }

      // Add change listener
      const input = item.querySelector('input, select, textarea');
      if (input) {
         input.addEventListener('change', () => handleSettingChange(fullKey, input));
         input.addEventListener('input', () => handleSettingChange(fullKey, input));
      }

      return item;
   }

   function handleSettingChange(key, input) {
      changedFields.add(key);

      // Check if this field requires restart
      if (restartRequiredFields.includes(key)) {
         if (settingsElements.restartNotice) {
            settingsElements.restartNotice.classList.remove('hidden');
         }
      }

      // If this is a model_list textarea, update any dropdowns that depend on it
      if (input.dataset.type === 'model_list') {
         updateDependentModelSelects(key, input.value);
      }
   }

   /**
    * Update model_default_select dropdowns when their source model_list changes
    */
   function updateDependentModelSelects(sourceKey, textareaValue) {
      // Parse models from textarea (same logic as collectConfigValues)
      const models = textareaValue
         .split('\n')
         .map((line) => line.trim())
         .filter((line) => line.length > 0);

      // Find all selects that depend on this source
      const selects = settingsElements.sectionsContainer.querySelectorAll(
         `select[data-source-key="${sourceKey}"]`
      );

      selects.forEach((select) => {
         const currentIdx = parseInt(select.value, 10) || 0;
         // Preserve selection if still valid, otherwise reset to 0
         const newIdx = currentIdx < models.length ? currentIdx : 0;

         // Rebuild options
         select.innerHTML =
            models.length > 0
               ? models
                    .map(
                       (model, idx) =>
                          `<option value="${idx}" ${idx === newIdx ? 'selected' : ''}>${escapeHtml(model)}</option>`
                    )
                    .join('')
               : '<option value="0">No models configured</option>';

         // Mark as changed if index changed
         if (newIdx !== currentIdx) {
            changedFields.add(select.dataset.key);
         }
      });
   }

   /* =============================================================================
    * Secrets Management
    * ============================================================================= */

   function updateSecretsStatus(secrets) {
      if (!secrets) return;

      const updateStatus = (el, isSet) => {
         if (!el) return;
         el.textContent = isSet ? 'Set' : 'Not set';
         el.className = `secret-status ${isSet ? 'is-set' : 'not-set'}`;
      };

      updateStatus(settingsElements.statusOpenai, secrets.openai_api_key);
      updateStatus(settingsElements.statusClaude, secrets.claude_api_key);
      updateStatus(settingsElements.statusGemini, secrets.gemini_api_key);
      updateStatus(settingsElements.statusMqttUser, secrets.mqtt_username);
      updateStatus(settingsElements.statusMqttPass, secrets.mqtt_password);
   }

   function collectConfigValues() {
      const config = {};
      const inputs = settingsElements.sectionsContainer.querySelectorAll('[data-key]');

      inputs.forEach((input) => {
         const key = input.dataset.key;
         const parts = key.split('.');

         let value;
         if (input.type === 'checkbox') {
            value = input.checked;
         } else if (input.type === 'number') {
            value = input.value !== '' ? parseFloat(input.value) : null;
         } else if (input.dataset.type === 'model_list' || input.dataset.type === 'string_list') {
            // Convert newline-separated text to array, filtering empty lines
            value = input.value
               .split('\n')
               .map((line) => line.trim())
               .filter((line) => line.length > 0);
         } else if (input.dataset.type === 'model_default_select') {
            // Parse as integer index
            value = parseInt(input.value, 10) || 0;
         } else {
            value = input.value;
         }

         // Build nested structure
         let obj = config;
         for (let i = 0; i < parts.length - 1; i++) {
            if (!obj[parts[i]]) obj[parts[i]] = {};
            obj = obj[parts[i]];
         }
         obj[parts[parts.length - 1]] = value;
      });

      // AI name should always be lowercase (for wake word detection)
      if (config.general && config.general.ai_name) {
         config.general.ai_name = config.general.ai_name.toLowerCase();
      }

      // Map virtual tool_calling section to llm.tools.mode
      // The UI has a separate "Tool Calling" section, but backend uses llm.tools.mode
      if (config.tool_calling && config.tool_calling.mode !== undefined) {
         if (!config.llm) config.llm = {};
         if (!config.llm.tools) config.llm.tools = {};
         config.llm.tools.mode = config.tool_calling.mode;
         // Remove the virtual tool_calling section (not in backend config)
         delete config.tool_calling;
      }

      return config;
   }

   function saveConfig() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) {
         console.error('WebSocket not connected');
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Cannot save: Not connected to server', 'error');
         }
         return;
      }

      const config = collectConfigValues();

      // Show loading state (H8)
      const btn = settingsElements.saveConfigBtn;
      if (btn) {
         btn.disabled = true;
         btn.dataset.originalText = btn.textContent;
         btn.textContent = 'Saving...';
      }

      DawnWS.send({
         type: 'set_config',
         payload: config,
      });
   }

   function saveSecrets() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) {
         console.error('WebSocket not connected');
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Cannot save: Not connected to server', 'error');
         }
         return;
      }

      const secrets = {};

      // Only send non-empty values (don't overwrite with empty)
      if (settingsElements.secretOpenai && settingsElements.secretOpenai.value) {
         secrets.openai_api_key = settingsElements.secretOpenai.value;
      }
      if (settingsElements.secretClaude && settingsElements.secretClaude.value) {
         secrets.claude_api_key = settingsElements.secretClaude.value;
      }
      if (settingsElements.secretGemini && settingsElements.secretGemini.value) {
         secrets.gemini_api_key = settingsElements.secretGemini.value;
      }
      if (settingsElements.secretMqttUser && settingsElements.secretMqttUser.value) {
         secrets.mqtt_username = settingsElements.secretMqttUser.value;
      }
      if (settingsElements.secretMqttPass && settingsElements.secretMqttPass.value) {
         secrets.mqtt_password = settingsElements.secretMqttPass.value;
      }

      if (Object.keys(secrets).length === 0) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('No secrets entered to save', 'warning');
         }
         return;
      }

      DawnWS.send({
         type: 'set_secrets',
         payload: secrets,
      });

      // Clear inputs after sending
      if (settingsElements.secretOpenai) settingsElements.secretOpenai.value = '';
      if (settingsElements.secretClaude) settingsElements.secretClaude.value = '';
      if (settingsElements.secretGemini) settingsElements.secretGemini.value = '';
      if (settingsElements.secretMqttUser) settingsElements.secretMqttUser.value = '';
      if (settingsElements.secretMqttPass) settingsElements.secretMqttPass.value = '';
   }

   function handleSetConfigResponse(payload) {
      // Restore button state (H8)
      const btn = settingsElements.saveConfigBtn;
      if (btn) {
         btn.disabled = false;
         btn.textContent = btn.dataset.originalText || 'Save Configuration';
      }

      if (payload.success) {
         // Check if any changed fields require restart
         const restartFields = getChangedRestartRequiredFields();
         if (restartFields.length > 0) {
            showRestartConfirmation(restartFields);
         } else {
            if (typeof DawnToast !== 'undefined') {
               DawnToast.show('Configuration saved successfully!', 'success');
            }
         }

         // Clear changed fields tracking
         changedFields.clear();

         // Refresh config to update globalDefaults and other cached values
         requestConfig();
      } else {
         console.error('Failed to save config:', payload.error);
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(
               'Failed to save configuration: ' + (payload.error || 'Unknown error'),
               'error'
            );
         }
      }
   }

   function handleSetSecretsResponse(payload) {
      if (payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Secrets saved successfully!', 'success');
         }

         // Update status indicators
         if (payload.secrets) {
            updateSecretsStatus(payload.secrets);
         }

         // Refresh config to get updated secrets_path
         requestConfig();
      } else {
         console.error('Failed to save secrets:', payload.error);
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(
               'Failed to save secrets: ' + (payload.error || 'Unknown error'),
               'error'
            );
         }
      }
   }

   function getChangedRestartRequiredFields() {
      const restartFields = [];
      for (const field of changedFields) {
         if (restartRequiredFields.includes(field)) {
            restartFields.push(field);
         }
      }
      return restartFields;
   }

   function showRestartConfirmation(changedRestartFields) {
      const fieldList = changedRestartFields.map((f) => '  • ' + f).join('\n');
      const message =
         'Configuration saved successfully!\n\n' +
         'The following changes require a restart to take effect:\n' +
         fieldList +
         '\n\n' +
         'Do you want to restart DAWN now?';

      showConfirmModal(message, requestRestart, {
         title: 'Restart Required',
         okText: 'Restart Now',
         cancelText: 'Later',
      });
   }

   function requestRestart() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) {
         console.error('WebSocket not connected');
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Cannot restart: Not connected to server', 'error');
         }
         return;
      }

      DawnWS.send({ type: 'restart' });
   }

   function handleRestartResponse(payload) {
      if (payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(
               'DAWN is restarting. The page will attempt to reconnect automatically.',
               'info'
            );
         }
      } else {
         console.error('Restart failed:', payload.error);
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Failed to restart: ' + (payload.error || 'Unknown error'), 'error');
         }
      }
   }

   /* =============================================================================
    * Audio Device Management
    * ============================================================================= */

   function requestAudioDevices(backend) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) {
         console.error('WebSocket not connected');
         return;
      }

      DawnWS.send({
         type: 'get_audio_devices',
         payload: { backend: backend },
      });
   }

   function handleGetAudioDevicesResponse(payload) {
      audioDevicesCache.capture = payload.capture_devices || [];
      audioDevicesCache.playback = payload.playback_devices || [];

      // Update capture device field
      updateAudioDeviceField('capture_device', audioDevicesCache.capture);
      updateAudioDeviceField('playback_device', audioDevicesCache.playback);
   }

   function updateAudioDeviceField(fieldName, devices) {
      // ID matches how createSettingField generates it (underscores preserved)
      const inputId = `setting-audio-${fieldName}`;
      const input = document.getElementById(inputId);
      if (!input) return;

      const currentValue = input.value || '';
      const dataKey = input.dataset.key;
      const parent = input.parentElement;

      // Replace input with select
      const select = document.createElement('select');
      select.id = inputId;
      select.dataset.key = dataKey;

      // Add default option
      const defaultOpt = document.createElement('option');
      defaultOpt.value = '';
      defaultOpt.textContent = 'Default';
      select.appendChild(defaultOpt);

      // Add device options (devices can be strings or {id, name} objects)
      devices.forEach((device) => {
         const opt = document.createElement('option');
         // Handle both string devices and object devices
         const deviceId = typeof device === 'string' ? device : device.id;
         const deviceName = typeof device === 'string' ? device : device.name || device.id;
         opt.value = deviceId;
         opt.textContent = deviceName;
         if (deviceId === currentValue) {
            opt.selected = true;
         }
         select.appendChild(opt);
      });

      // Add change listener
      select.addEventListener('change', () => handleSettingChange(dataKey, select));

      // Replace input with select
      parent.replaceChild(select, input);
   }

   function updateAudioBackendState(backend) {
      // IDs match how createSettingField generates them (underscores preserved)
      const captureInput = document.getElementById('setting-audio-capture_device');
      const playbackInput = document.getElementById('setting-audio-playback_device');

      if (backend === 'auto') {
         // Grey out device fields when auto
         if (captureInput) {
            captureInput.disabled = true;
            captureInput.title = 'Device is auto-detected when backend is "auto"';
         }
         if (playbackInput) {
            playbackInput.disabled = true;
            playbackInput.title = 'Device is auto-detected when backend is "auto"';
         }
      } else {
         // Enable device fields and request available devices
         if (captureInput) {
            captureInput.disabled = false;
            captureInput.title = '';
         }
         if (playbackInput) {
            playbackInput.disabled = false;
            playbackInput.title = '';
         }

         // Request available devices for this backend
         requestAudioDevices(backend);
      }
   }

   function toggleSecretVisibility(targetId) {
      const input = document.getElementById(targetId);
      if (!input) return;

      input.type = input.type === 'password' ? 'text' : 'password';
   }

   /* =============================================================================
    * Modal Dialogs
    * ============================================================================= */

   /**
    * Focus trap for modals - keeps Tab key cycling within modal
    * @param {HTMLElement} modal - Modal element to trap focus in
    * @returns {Function} Cleanup function to remove event listener
    */
   function trapFocus(modal) {
      const focusableSelectors =
         'button, [href], input, select, textarea, [tabindex]:not([tabindex="-1"])';
      const focusableElements = modal.querySelectorAll(focusableSelectors);
      const firstFocusable = focusableElements[0];
      const lastFocusable = focusableElements[focusableElements.length - 1];

      function handleKeydown(e) {
         if (e.key !== 'Tab') return;

         if (e.shiftKey) {
            if (document.activeElement === firstFocusable) {
               e.preventDefault();
               lastFocusable.focus();
            }
         } else {
            if (document.activeElement === lastFocusable) {
               e.preventDefault();
               firstFocusable.focus();
            }
         }
      }

      modal.addEventListener('keydown', handleKeydown);

      // Focus first element
      if (firstFocusable) firstFocusable.focus();

      // Return cleanup function
      return () => modal.removeEventListener('keydown', handleKeydown);
   }

   /**
    * Show styled confirmation modal (replaces native confirm())
    * @param {string} message - Message to display
    * @param {Function} onConfirm - Callback if user confirms
    * @param {Object} options - Optional settings
    * @param {string} options.title - Modal title (default: "Confirm")
    * @param {string} options.okText - OK button text (default: "OK")
    * @param {string} options.cancelText - Cancel button text (default: "Cancel")
    * @param {boolean} options.danger - If true, styles OK button as danger/red
    */
   function showConfirmModal(message, onConfirm, options = {}) {
      const modal = document.getElementById('confirm-modal');
      if (!modal) {
         // Fallback to native confirm if modal not found
         if (confirm(message) && onConfirm) onConfirm();
         return;
      }

      const content = modal.querySelector('.modal-content');
      const titleEl = document.getElementById('confirm-modal-title');
      const messageEl = document.getElementById('confirm-modal-message');
      const okBtn = document.getElementById('confirm-modal-ok');
      const cancelBtn = document.getElementById('confirm-modal-cancel');

      // Set content
      if (titleEl) titleEl.textContent = options.title || 'Confirm';
      if (messageEl) messageEl.textContent = message;
      if (okBtn) okBtn.textContent = options.okText || 'OK';
      if (cancelBtn) cancelBtn.textContent = options.cancelText || 'Cancel';

      // Apply danger styling if requested
      if (content) {
         content.classList.toggle('confirm-danger', !!options.danger);
      }

      // Store callback and trigger element for focus restoration (M13)
      pendingConfirmCallback = onConfirm;
      modalTriggerElement = document.activeElement;

      // Show modal
      modal.classList.remove('hidden');
      confirmModalCleanup = trapFocus(modal);
   }

   /**
    * Hide confirmation modal
    * @param {boolean} confirmed - Whether user confirmed the action
    */
   function hideConfirmModal(confirmed) {
      const modal = document.getElementById('confirm-modal');
      if (modal) {
         modal.classList.add('hidden');
         if (confirmModalCleanup) {
            confirmModalCleanup();
            confirmModalCleanup = null;
         }
      }

      if (confirmed && pendingConfirmCallback) {
         pendingConfirmCallback();
      }
      pendingConfirmCallback = null;

      // Restore focus to trigger element (M13)
      if (modalTriggerElement && typeof modalTriggerElement.focus === 'function') {
         modalTriggerElement.focus();
         modalTriggerElement = null;
      }
   }

   /**
    * Initialize confirmation modal event handlers
    */
   function initConfirmModal() {
      const okBtn = document.getElementById('confirm-modal-ok');
      const cancelBtn = document.getElementById('confirm-modal-cancel');
      const modal = document.getElementById('confirm-modal');

      if (okBtn) {
         okBtn.addEventListener('click', () => hideConfirmModal(true));
      }
      if (cancelBtn) {
         cancelBtn.addEventListener('click', () => hideConfirmModal(false));
      }

      // Close on backdrop click
      if (modal) {
         modal.addEventListener('click', (e) => {
            if (e.target === modal) hideConfirmModal(false);
         });
      }

      // Close on Escape key
      document.addEventListener('keydown', (e) => {
         if (e.key === 'Escape' && modal && !modal.classList.contains('hidden')) {
            hideConfirmModal(false);
         }
      });
   }

   /**
    * Show styled input modal (replaces native prompt())
    * @param {string} message - Message to display
    * @param {string} defaultValue - Default value for input field
    * @param {Function} onSubmit - Callback with input value if user submits
    * @param {Object} options - Optional settings
    * @param {string} options.title - Modal title (default: "Enter Value")
    * @param {string} options.okText - OK button text (default: "OK")
    * @param {string} options.cancelText - Cancel button text (default: "Cancel")
    * @param {string} options.placeholder - Input placeholder text
    */
   function showInputModal(message, defaultValue, onSubmit, options = {}) {
      const modal = document.getElementById('input-modal');
      if (!modal) {
         // Fallback to native prompt if modal not found
         const result = prompt(message, defaultValue);
         if (result !== null && onSubmit) onSubmit(result);
         return;
      }

      const titleEl = document.getElementById('input-modal-title');
      const messageEl = document.getElementById('input-modal-message');
      const inputEl = document.getElementById('input-modal-input');
      const okBtn = document.getElementById('input-modal-ok');
      const cancelBtn = document.getElementById('input-modal-cancel');

      // Set content
      if (titleEl) titleEl.textContent = options.title || 'Enter Value';
      if (messageEl) {
         messageEl.textContent = message || '';
         messageEl.style.display = message ? 'block' : 'none';
      }
      if (inputEl) {
         inputEl.value = defaultValue || '';
         inputEl.placeholder = options.placeholder || '';
      }
      if (okBtn) okBtn.textContent = options.okText || 'OK';
      if (cancelBtn) cancelBtn.textContent = options.cancelText || 'Cancel';

      // Store callback and trigger element for focus restoration (M13)
      pendingInputCallback = onSubmit;
      modalTriggerElement = document.activeElement;

      // Show modal and focus input
      modal.classList.remove('hidden');
      inputModalCleanup = trapFocus(modal);

      // Focus and select input text
      if (inputEl) {
         inputEl.focus();
         inputEl.select();
      }
   }

   /**
    * Hide input modal
    * @param {boolean} submitted - Whether user submitted the input
    */
   function hideInputModal(submitted) {
      const modal = document.getElementById('input-modal');
      const inputEl = document.getElementById('input-modal-input');

      if (modal) {
         modal.classList.add('hidden');
         if (inputModalCleanup) {
            inputModalCleanup();
            inputModalCleanup = null;
         }
      }

      if (submitted && pendingInputCallback && inputEl) {
         pendingInputCallback(inputEl.value);
      }
      pendingInputCallback = null;

      // Restore focus to trigger element (M13)
      if (modalTriggerElement && typeof modalTriggerElement.focus === 'function') {
         modalTriggerElement.focus();
         modalTriggerElement = null;
      }
   }

   /**
    * Initialize input modal event handlers
    */
   function initInputModal() {
      const okBtn = document.getElementById('input-modal-ok');
      const cancelBtn = document.getElementById('input-modal-cancel');
      const inputEl = document.getElementById('input-modal-input');
      const modal = document.getElementById('input-modal');

      if (okBtn) {
         okBtn.addEventListener('click', () => hideInputModal(true));
      }
      if (cancelBtn) {
         cancelBtn.addEventListener('click', () => hideInputModal(false));
      }

      // Submit on Enter key in input field
      if (inputEl) {
         inputEl.addEventListener('keydown', (e) => {
            if (e.key === 'Enter') {
               e.preventDefault();
               hideInputModal(true);
            }
         });
      }

      // Close on backdrop click
      if (modal) {
         modal.addEventListener('click', (e) => {
            if (e.target === modal) hideInputModal(false);
         });
      }

      // Close on Escape key
      document.addEventListener('keydown', (e) => {
         if (e.key === 'Escape' && modal && !modal.classList.contains('hidden')) {
            hideInputModal(false);
         }
      });
   }

   /* =============================================================================
    * System Prompt Display
    * ============================================================================= */

   function handleSystemPromptResponse(payload) {
      if (!payload.success) {
         console.warn('Failed to get system prompt:', payload.error);
         return;
      }

      // Remove any existing system prompt entry
      const existing = document.getElementById('system-prompt-entry');
      if (existing) {
         existing.remove();
      }

      // Create collapsible system prompt entry
      const entry = document.createElement('div');
      entry.id = 'system-prompt-entry';
      entry.className = 'transcript-entry debug system-prompt';

      const promptLength = payload.length || payload.prompt.length;
      const tokenEstimate = Math.round(promptLength / 4); // Rough estimate

      entry.innerHTML = `
      <div class="system-prompt-header">
        <span class="system-prompt-icon">&#x2699;</span>
        <span class="system-prompt-title">System Prompt</span>
        <span class="system-prompt-stats">${promptLength.toLocaleString()} chars (~${tokenEstimate.toLocaleString()} tokens)</span>
        <span class="system-prompt-toggle">&#x25BC;</span>
      </div>
      <div class="system-prompt-content">
        <pre>${escapeHtml(payload.prompt)}</pre>
      </div>
    `;

      // Add click handler for expand/collapse
      const header = entry.querySelector('.system-prompt-header');
      header.addEventListener('click', function () {
         entry.classList.toggle('expanded');
      });

      // Insert at the top of the transcript (after placeholder if present)
      const transcript =
         typeof DawnElements !== 'undefined'
            ? DawnElements.transcript
            : document.getElementById('transcript');
      if (transcript) {
         const placeholder = transcript.querySelector('.transcript-placeholder');
         if (placeholder) {
            placeholder.after(entry);
         } else {
            transcript.prepend(entry);
         }
      }

      // Show only if debug mode is on
      if (typeof DawnState !== 'undefined' && !DawnState.getDebugMode()) {
         entry.style.display = 'none';
      }
   }

   /* =============================================================================
    * LLM Runtime Controls
    * ============================================================================= */

   // Cached model lists from config and local LLM (includes default indices)
   let cloudModelLists = {
      openai: [],
      claude: [],
      gemini: [],
      openaiDefaultIdx: 0,
      claudeDefaultIdx: 0,
      geminiDefaultIdx: 0,
   };
   let localModelList = [];
   let localProviderType = 'unknown'; // 'ollama', 'llama.cpp', 'Generic', 'Unknown'

   function initLlmControls() {
      const typeSelect = document.getElementById('llm-type-select');
      const providerSelect = document.getElementById('llm-provider-select');
      const modelSelect = document.getElementById('llm-model-select');

      if (typeSelect) {
         typeSelect.addEventListener('change', () => {
            const newType = typeSelect.value;
            setSessionLlm({ type: newType });

            // Request local models when switching to local mode
            if (newType === 'local') {
               requestLocalModels();
            } else {
               // Switching to cloud: restore provider dropdown and update model
               if (providerSelect) {
                  // Rebuild cloud provider options
                  providerSelect.innerHTML = '';
                  providerSelect.disabled = false;
                  if (cloudModelLists.openai.length > 0) {
                     const opt = document.createElement('option');
                     opt.value = 'openai';
                     opt.textContent = 'OpenAI';
                     providerSelect.appendChild(opt);
                  }
                  if (cloudModelLists.claude.length > 0) {
                     const opt = document.createElement('option');
                     opt.value = 'claude';
                     opt.textContent = 'Claude';
                     providerSelect.appendChild(opt);
                  }
                  if (cloudModelLists.gemini.length > 0) {
                     const opt = document.createElement('option');
                     opt.value = 'gemini';
                     opt.textContent = 'Gemini';
                     providerSelect.appendChild(opt);
                  }
                  // Select default provider from global defaults
                  const defaultProvider = globalDefaults.provider || 'openai';
                  if (providerSelect.querySelector(`option[value="${defaultProvider}"]`)) {
                     providerSelect.value = defaultProvider;
                  }
                  // Also send provider to session
                  setSessionLlm({ provider: providerSelect.value });
               }
               // Populate cloud models and send model to session
               updateModelDropdownForCloud(true);
            }
         });
      }

      if (providerSelect) {
         providerSelect.addEventListener('change', () => {
            setSessionLlm({ provider: providerSelect.value });
            // Update model dropdown for new provider and send model to session
            updateModelDropdownForCloud(true);
         });
      }

      if (modelSelect) {
         modelSelect.addEventListener('change', () => {
            if (modelSelect.value) {
               setSessionLlm({ model: modelSelect.value });
            }
         });
      }
   }

   /* =============================================================================
    * Per-Conversation LLM Settings
    * ============================================================================= */

   /**
    * Set the locked state for conversation LLM controls
    */
   function setConversationLlmLocked(locked) {
      conversationLlmState.locked = locked;
      const container = document.getElementById('llm-conversation-controls');
      const indicator = document.getElementById('llm-lock-indicator');
      const reasoningSelect = document.getElementById('reasoning-mode-select');
      const toolsSelect = document.getElementById('tools-mode-select');

      if (container) {
         container.classList.toggle('locked', locked);
      }
      if (reasoningSelect) {
         reasoningSelect.disabled = locked;
      }
      if (toolsSelect) {
         toolsSelect.disabled = locked;
      }
      if (indicator) {
         indicator.classList.toggle('hidden', !locked);
      }
   }

   /**
    * Initialize per-conversation LLM controls
    */
   function initConversationLlmControls() {
      const reasoningSelect = document.getElementById('reasoning-mode-select');
      const toolsSelect = document.getElementById('tools-mode-select');

      if (reasoningSelect) {
         reasoningSelect.addEventListener('change', () => {
            conversationLlmState.thinking_mode = reasoningSelect.value;
            // Immediately update session config so thinking_mode takes effect
            if (!conversationLlmState.locked) {
               setSessionLlm({ thinking_mode: reasoningSelect.value });
            }
         });
      }

      if (toolsSelect) {
         toolsSelect.addEventListener('change', () => {
            conversationLlmState.tools_mode = toolsSelect.value;
            // Immediately update session config so tool_mode takes effect
            if (!conversationLlmState.locked) {
               setSessionLlm({ tool_mode: toolsSelect.value });
            }
         });
      }

      // Initial session defaults are applied after config loads via applyGlobalDefaultsToControls()

      // Tools help button popup
      const toolsHelpBtn = document.getElementById('tools-help-btn');
      const toolsHelpPopup = document.getElementById('tools-help-popup');

      if (toolsHelpBtn && toolsHelpPopup) {
         // Toggle popup on button click
         toolsHelpBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            toolsHelpPopup.classList.toggle('hidden');
         });

         // Close popup when clicking outside
         // NOTE: These document-level listeners are intentionally not removed.
         // The popup is a singleton that lives for the entire session, so cleanup
         // is not necessary and the handlers have negligible overhead when inactive.
         document.addEventListener('click', (e) => {
            if (!toolsHelpPopup.contains(e.target) && e.target !== toolsHelpBtn) {
               toolsHelpPopup.classList.add('hidden');
            }
         });

         // Close popup on Escape key
         document.addEventListener('keydown', (e) => {
            if (e.key === 'Escape' && !toolsHelpPopup.classList.contains('hidden')) {
               toolsHelpPopup.classList.add('hidden');
            }
         });
      }
   }

   /**
    * Reset conversation LLM controls for a new conversation
    * Resets all controls (mode, provider, model, reasoning, tools) to global defaults
    */
   function resetConversationLlmControls() {
      setConversationLlmLocked(false);

      const typeSelect = document.getElementById('llm-type-select');
      const providerSelect = document.getElementById('llm-provider-select');
      const modelSelect = document.getElementById('llm-model-select');
      const reasoningSelect = document.getElementById('reasoning-mode-select');
      const toolsSelect = document.getElementById('tools-mode-select');

      // Reset type (local/cloud)
      if (typeSelect) {
         typeSelect.value = globalDefaults.type;
      }

      // Build session reset payload
      const sessionReset = {
         type: globalDefaults.type,
         tool_mode: globalDefaults.tools_mode,
         thinking_mode: globalDefaults.thinking_mode,
      };

      // Reset provider and model based on type
      if (globalDefaults.type === 'cloud') {
         if (providerSelect) {
            providerSelect.value = globalDefaults.provider;
         }

         // Reset model to provider's default
         let defaultModel;
         if (globalDefaults.provider === 'claude') {
            defaultModel = globalDefaults.claude_model;
         } else if (globalDefaults.provider === 'gemini') {
            defaultModel = globalDefaults.gemini_model;
         } else {
            defaultModel = globalDefaults.openai_model;
         }

         if (modelSelect && defaultModel) {
            // Update dropdown options first
            updateModelDropdownForCloud();
            modelSelect.value = defaultModel;
         }

         sessionReset.cloud_provider = globalDefaults.provider;
         sessionReset.model = defaultModel;
      }

      // Reset reasoning dropdown to global default
      if (reasoningSelect) {
         reasoningSelect.value = globalDefaults.thinking_mode;
         conversationLlmState.thinking_mode = globalDefaults.thinking_mode;
         sessionReset.thinking_mode = globalDefaults.thinking_mode;
      }

      // Reset tools dropdown
      if (toolsSelect) {
         toolsSelect.value = globalDefaults.tools_mode;
         conversationLlmState.tools_mode = globalDefaults.tools_mode;
      }

      // Update runtime state
      llmRuntimeState.type = globalDefaults.type;
      llmRuntimeState.provider = globalDefaults.provider;
      if (sessionReset.model) {
         llmRuntimeState.model = sessionReset.model;
      }

      // Send reset to session
      setSessionLlm(sessionReset);
   }

   /**
    * Apply LLM settings from a loaded conversation
    */
   function applyConversationLlmSettings(settings, isLocked) {
      const reasoningSelect = document.getElementById('reasoning-mode-select');
      const toolsSelect = document.getElementById('tools-mode-select');

      if (settings) {
         if (settings.thinking_mode && reasoningSelect) {
            reasoningSelect.value = settings.thinking_mode;
            conversationLlmState.thinking_mode = settings.thinking_mode;
         }

         if (settings.tools_mode && toolsSelect) {
            toolsSelect.value = settings.tools_mode;
            conversationLlmState.tools_mode = settings.tools_mode;
         }

         // Apply provider/model settings to sync UI with loaded conversation
         // The backend has already restored the LLM config - this syncs the frontend UI
         if (settings.llm_type || settings.cloud_provider || settings.model) {
            const changes = {};
            if (settings.llm_type) changes.type = settings.llm_type;
            if (settings.cloud_provider) changes.provider = settings.cloud_provider;
            if (settings.model) changes.model = settings.model;

            // Show loading state while syncing
            const llmControls = document.getElementById('llm-controls');
            if (llmControls) {
               llmControls.classList.add('llm-syncing');
            }

            // Request backend to confirm settings - response will update UI via updateLlmControls
            if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
               DawnWS.send({
                  type: 'set_session_llm',
                  payload: changes,
               });
            }
         }
      }

      setConversationLlmLocked(isLocked);
   }

   /**
    * Get current conversation LLM settings for locking
    */
   function getConversationLlmSettings() {
      return {
         llm_type: llmRuntimeState.type,
         cloud_provider: llmRuntimeState.provider,
         model: llmRuntimeState.model,
         tools_mode: conversationLlmState.tools_mode,
         thinking_mode: conversationLlmState.thinking_mode,
      };
   }

   /**
    * Lock conversation LLM settings (called when first message is sent)
    */
   function lockConversationLlmSettings(conversationId) {
      if (conversationLlmState.locked) {
         return; // Already locked
      }

      const settings = getConversationLlmSettings();
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({
            type: 'lock_conversation_llm',
            payload: {
               conversation_id: conversationId,
               llm_settings: settings,
            },
         });
      }

      // Optimistically lock the UI
      setConversationLlmLocked(true);
   }

   let detectLocalProviderTimeout = null;
   const DETECT_TIMEOUT_MS = 10000; // 10 seconds

   function requestLocalModels() {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({ type: 'list_llm_models' });

         // Set timeout to show "Unknown" if detection takes too long
         if (detectLocalProviderTimeout) {
            clearTimeout(detectLocalProviderTimeout);
         }
         detectLocalProviderTimeout = setTimeout(() => {
            const providerSelect = document.getElementById('llm-provider-select');
            const typeSelect = document.getElementById('llm-type-select');
            if (
               providerSelect &&
               typeSelect &&
               typeSelect.value === 'local' &&
               providerSelect.value === 'detecting'
            ) {
               providerSelect.innerHTML = '';
               const opt = document.createElement('option');
               opt.value = 'unknown';
               opt.textContent = 'Unknown';
               providerSelect.appendChild(opt);
               providerSelect.title = 'Could not detect local provider';
               console.warn('Local provider detection timed out');
            }
         }, DETECT_TIMEOUT_MS);
      }
   }

   function handleListLlmModelsResponse(payload) {
      // Clear detection timeout since we received a response
      if (detectLocalProviderTimeout) {
         clearTimeout(detectLocalProviderTimeout);
         detectLocalProviderTimeout = null;
      }

      localModelList = payload.models || [];
      localProviderType = payload.provider || 'Unknown';

      // Update provider dropdown to show detected local provider
      const providerSelect = document.getElementById('llm-provider-select');
      const typeSelect = document.getElementById('llm-type-select');

      if (providerSelect && typeSelect && typeSelect.value === 'local') {
         providerSelect.innerHTML = '';
         const opt = document.createElement('option');
         opt.value = localProviderType.toLowerCase();
         opt.textContent = localProviderType;
         providerSelect.appendChild(opt);
         providerSelect.disabled = true;
         providerSelect.title = `Local provider: ${localProviderType}`;
      }

      const modelSelect = document.getElementById('llm-model-select');
      if (!modelSelect) return;

      // Only update model dropdown if we're in local mode
      if (typeSelect && typeSelect.value !== 'local') return;

      modelSelect.innerHTML = '';

      // llama.cpp doesn't support model switching (only shows loaded model)
      if (localProviderType === 'llama.cpp') {
         const opt = document.createElement('option');
         opt.value = payload.current_model || '';
         opt.textContent = payload.current_model || '(server default)';
         modelSelect.appendChild(opt);
         modelSelect.disabled = true;
         modelSelect.title = 'Model switching not supported with llama.cpp';
         // Update session with the loaded model (only if changed to avoid feedback loop)
         if (payload.current_model && llmRuntimeState.model !== payload.current_model) {
            llmRuntimeState.model = payload.current_model;
            setSessionLlm({ model: payload.current_model });
         }
         return;
      }

      // Ollama or Generic - enable selection
      if (localModelList.length === 0) {
         const opt = document.createElement('option');
         opt.value = '';
         opt.textContent = 'No models available';
         modelSelect.appendChild(opt);
         modelSelect.disabled = true;
         return;
      }

      localModelList.forEach((model) => {
         const opt = document.createElement('option');
         opt.value = model.name;
         opt.textContent = model.name + (model.loaded ? ' (loaded)' : '');
         modelSelect.appendChild(opt);
      });

      // Select current model and update session (only if changed to avoid feedback loop)
      if (payload.current_model) {
         modelSelect.value = payload.current_model;
         if (llmRuntimeState.model !== payload.current_model) {
            llmRuntimeState.model = payload.current_model;
            setSessionLlm({ model: payload.current_model });
         }
      }
      modelSelect.disabled = false;
      modelSelect.title = 'Select local model';
   }

   function updateModelDropdownForCloud(sendToSession = false) {
      const modelSelect = document.getElementById('llm-model-select');
      const providerSelect = document.getElementById('llm-provider-select');
      if (!modelSelect || !providerSelect) return;

      const provider = providerSelect.value?.toLowerCase() || 'openai';
      const models = cloudModelLists[provider] || [];

      modelSelect.innerHTML = '';

      if (models.length === 0) {
         const opt = document.createElement('option');
         opt.value = '';
         opt.textContent = 'Configure in settings';
         modelSelect.appendChild(opt);
         modelSelect.disabled = true;
         modelSelect.title = 'Add models in Settings > LLM > Cloud Settings';
         return;
      }

      models.forEach((model) => {
         const opt = document.createElement('option');
         opt.value = model;
         opt.textContent = model;
         modelSelect.appendChild(opt);
      });

      // Select current model if in list, otherwise use provider's default
      const currentModel = llmRuntimeState?.model;
      if (currentModel && models.includes(currentModel)) {
         modelSelect.value = currentModel;
      } else {
         // Use default index for this provider, fall back to first model
         let defaultIdx;
         if (provider === 'claude') {
            defaultIdx = cloudModelLists.claudeDefaultIdx;
         } else if (provider === 'gemini') {
            defaultIdx = cloudModelLists.geminiDefaultIdx;
         } else {
            defaultIdx = cloudModelLists.openaiDefaultIdx;
         }
         // Ensure index is valid
         if (defaultIdx >= 0 && defaultIdx < models.length) {
            modelSelect.value = models[defaultIdx];
         } else {
            modelSelect.value = models[0];
         }
      }

      modelSelect.disabled = false;
      modelSelect.title = 'Select cloud model';

      // Send selected model to session if requested (e.g., after provider switch)
      // Only send if model actually changed to avoid feedback loops
      if (sendToSession && modelSelect.value && llmRuntimeState.model !== modelSelect.value) {
         llmRuntimeState.model = modelSelect.value;
         setSessionLlm({ model: modelSelect.value });
      }
   }

   function updateCloudModelLists(config) {
      // Extract model lists and default indices from config response
      if (config?.llm?.cloud) {
         const cloud = config.llm.cloud;
         cloudModelLists.openai = cloud.openai_models || [];
         cloudModelLists.claude = cloud.claude_models || [];
         cloudModelLists.gemini = cloud.gemini_models || [];
         cloudModelLists.openaiDefaultIdx = cloud.openai_default_model_idx || 0;
         cloudModelLists.claudeDefaultIdx = cloud.claude_default_model_idx || 0;
         cloudModelLists.geminiDefaultIdx = cloud.gemini_default_model_idx || 0;
      }
   }

   /**
    * Extract global defaults from config for resetting new conversations
    */
   function extractGlobalDefaults(config) {
      if (!config) return;

      // LLM type (local/cloud)
      if (config.llm?.type) {
         globalDefaults.type = config.llm.type;
      }

      // Cloud provider
      if (config.llm?.cloud?.provider) {
         globalDefaults.provider = config.llm.cloud.provider;
      }

      // Default models (resolve idx to model name)
      if (config.llm?.cloud) {
         const cloud = config.llm.cloud;
         const openaiModels = cloud.openai_models || [];
         const claudeModels = cloud.claude_models || [];
         const geminiModels = cloud.gemini_models || [];
         const openaiIdx = cloud.openai_default_model_idx || 0;
         const claudeIdx = cloud.claude_default_model_idx || 0;
         const geminiIdx = cloud.gemini_default_model_idx || 0;

         globalDefaults.openai_model = openaiModels[openaiIdx] || openaiModels[0] || '';
         globalDefaults.claude_model = claudeModels[claudeIdx] || claudeModels[0] || '';
         globalDefaults.gemini_model = geminiModels[geminiIdx] || geminiModels[0] || '';
      }

      // Tools mode
      if (config.llm?.tools?.mode) {
         globalDefaults.tools_mode = config.llm.tools.mode;
      }

      // Thinking mode (Claude/local)
      if (config.llm?.thinking?.mode) {
         globalDefaults.thinking_mode = config.llm.thinking.mode;
      }

      // Reasoning effort (OpenAI o-series/GPT-5)
      if (config.llm?.thinking?.reasoning_effort) {
         globalDefaults.reasoning_effort = config.llm.thinking.reasoning_effort;
      }
   }

   /**
    * Apply global defaults to conversation LLM controls
    * Called after config loads to set initial UI state
    */
   function applyGlobalDefaultsToControls() {
      const reasoningSelect = document.getElementById('reasoning-mode-select');
      const toolsSelect = document.getElementById('tools-mode-select');

      if (reasoningSelect) {
         reasoningSelect.value = globalDefaults.thinking_mode;
         conversationLlmState.thinking_mode = globalDefaults.thinking_mode;
      }

      if (toolsSelect) {
         toolsSelect.value = globalDefaults.tools_mode;
         conversationLlmState.tools_mode = globalDefaults.tools_mode;
      }

      // Send initial defaults to session
      setSessionLlm({
         tool_mode: globalDefaults.tools_mode,
         thinking_mode: globalDefaults.thinking_mode,
      });
   }

   function updateLlmControls(runtime) {
      llmRuntimeState = { ...llmRuntimeState, ...runtime };

      const typeSelect = document.getElementById('llm-type-select');
      const providerSelect = document.getElementById('llm-provider-select');

      if (typeSelect) {
         typeSelect.value = runtime.type || 'cloud';
      }

      if (providerSelect) {
         // Update available options based on API key availability
         providerSelect.innerHTML = '';

         if (runtime.openai_available) {
            const opt = document.createElement('option');
            opt.value = 'openai';
            opt.textContent = 'OpenAI';
            providerSelect.appendChild(opt);
         }

         if (runtime.claude_available) {
            const opt = document.createElement('option');
            opt.value = 'claude';
            opt.textContent = 'Claude';
            providerSelect.appendChild(opt);
         }

         if (runtime.gemini_available) {
            const opt = document.createElement('option');
            opt.value = 'gemini';
            opt.textContent = 'Gemini';
            providerSelect.appendChild(opt);
         }

         // If no providers available, show disabled message
         if (!runtime.openai_available && !runtime.claude_available && !runtime.gemini_available) {
            const opt = document.createElement('option');
            opt.value = '';
            opt.textContent = 'No API keys configured';
            opt.disabled = true;
            providerSelect.appendChild(opt);
            providerSelect.disabled = true;
         } else {
            providerSelect.disabled = false;
            // Set current value
            const currentProvider = runtime.provider?.toLowerCase() || 'openai';
            if (providerSelect.querySelector(`option[value="${currentProvider}"]`)) {
               providerSelect.value = currentProvider;
            }
         }
      }

      // Enable/disable provider selector based on LLM type
      if (providerSelect) {
         if (runtime.type === 'local') {
            // Show detecting state until list_llm_models_response arrives
            providerSelect.innerHTML = '';
            const opt = document.createElement('option');
            opt.value = 'detecting';
            opt.textContent = 'Detecting...';
            providerSelect.appendChild(opt);
            providerSelect.disabled = true;
            providerSelect.title = 'Auto-detecting local provider';
         } else if (
            runtime.openai_available ||
            runtime.claude_available ||
            runtime.gemini_available
         ) {
            providerSelect.disabled = false;
            providerSelect.title = 'Switch cloud provider';
         }
      }

      // Update model dropdown based on mode
      if (runtime.type === 'local') {
         // Request local models to populate dropdown
         requestLocalModels();
      } else {
         // Populate cloud models dropdown
         updateModelDropdownForCloud();
      }
   }

   function setSessionLlm(changes) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) {
         console.error('WebSocket not connected');
         return;
      }

      DawnWS.send({
         type: 'set_session_llm',
         payload: changes,
      });
   }

   function handleSetSessionLlmResponse(payload) {
      // Clear loading state
      const llmControls = document.getElementById('llm-controls');
      if (llmControls) {
         llmControls.classList.remove('llm-syncing');
      }

      if (payload.success) {
         updateLlmControls(payload);
      } else {
         console.error('Failed to update session LLM:', payload.error);
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Failed to switch LLM: ' + (payload.error || 'Unknown error'), 'error');
         }

         // Revert controls to actual state
         if (llmRuntimeState) {
            updateLlmControls(llmRuntimeState);
         }
      }
   }

   /* =============================================================================
    * Utility Functions
    * ============================================================================= */

   function escapeAttr(str) {
      return String(str)
         .replace(/&/g, '&amp;')
         .replace(/"/g, '&quot;')
         .replace(/'/g, '&#39;')
         .replace(/</g, '&lt;')
         .replace(/>/g, '&gt;');
   }

   function escapeHtml(str) {
      // Use shared implementation from format.js
      return DawnFormat.escapeHtml(str);
   }

   /**
    * Get a nested value from an object using dot notation
    * @param {Object} obj - The object to traverse
    * @param {string} path - Dot-separated path (e.g., 'llm.cloud.openai_models')
    * @returns {*} The value at the path, or undefined if not found
    */
   function getNestedValue(obj, path) {
      if (!obj || !path) return undefined;
      const parts = path.split('.');
      let current = obj;
      for (const part of parts) {
         if (current === null || current === undefined) return undefined;
         current = current[part];
      }
      return current;
   }

   /**
    * Format a number for display, removing floating-point precision artifacts.
    * Rounds to 6 significant digits to clean up values like 0.9200000166893005 → 0.92
    */
   function formatNumber(value) {
      if (value === undefined || value === null || value === '') return '';
      const num = Number(value);
      if (isNaN(num)) return '';
      // Use toPrecision to limit significant digits, then parseFloat to remove trailing zeros
      return parseFloat(num.toPrecision(6));
   }

   /* =============================================================================
    * Event Listener Initialization
    * ============================================================================= */

   function initListeners() {
      // Open button
      if (settingsElements.openBtn) {
         settingsElements.openBtn.addEventListener('click', open);
      }

      // Close button
      if (settingsElements.closeBtn) {
         settingsElements.closeBtn.addEventListener('click', close);
      }

      // Overlay click to close
      if (settingsElements.overlay) {
         settingsElements.overlay.addEventListener('click', close);
      }

      // Save config button
      if (settingsElements.saveConfigBtn) {
         settingsElements.saveConfigBtn.addEventListener('click', saveConfig);
      }

      // Save secrets button
      if (settingsElements.saveSecretsBtn) {
         settingsElements.saveSecretsBtn.addEventListener('click', saveSecrets);
      }

      // Reset button
      if (settingsElements.resetBtn) {
         settingsElements.resetBtn.addEventListener('click', () => {
            showConfirmModal(
               'Reset all settings to defaults?\n\nThis will reload the current configuration.',
               requestConfig,
               {
                  title: 'Reset Configuration',
                  okText: 'Reset',
               }
            );
         });
      }

      // Secret toggle buttons
      document.querySelectorAll('.secret-toggle').forEach((btn) => {
         btn.addEventListener('click', () => {
            const targetId = btn.dataset.target;
            if (targetId) {
               toggleSecretVisibility(targetId);
            }
         });
      });

      // Password toggle buttons (modals)
      document.querySelectorAll('.password-toggle').forEach((btn) => {
         btn.addEventListener('click', () => {
            const targetId = btn.dataset.target;
            if (targetId) {
               toggleSecretVisibility(targetId);
            }
         });
      });

      // Section header toggle (toggle on parent .settings-section)
      document.querySelectorAll('.section-header').forEach((header) => {
         // Skip user-management-section - has its own handler
         if (header.closest('#user-management-section')) {
            return;
         }
         header.addEventListener('click', () => {
            const section = header.closest('.settings-section');
            if (section) {
               section.classList.toggle('collapsed');
            }
         });
      });

      // Escape key to close (but not if a modal is open)
      document.addEventListener('keydown', (e) => {
         if (
            e.key === 'Escape' &&
            settingsElements.panel &&
            !settingsElements.panel.classList.contains('hidden')
         ) {
            // Don't close settings if a modal dialog is open
            const openModal = document.querySelector('.modal:not(.hidden)');
            if (!openModal) {
               close();
            }
         }
      });

      // LLM quick controls event listeners
      initLlmControls();

      // SmartThings initialization
      if (typeof DawnSmartThings !== 'undefined') {
         DawnSmartThings.setElements({
            stStatusIndicator: settingsElements.stStatusIndicator,
            stStatusText: settingsElements.stStatusText,
            stNotConfigured: settingsElements.stNotConfigured,
            stConnectBtn: settingsElements.stConnectBtn,
            stRefreshBtn: settingsElements.stRefreshBtn,
            stDisconnectBtn: settingsElements.stDisconnectBtn,
            stDevicesCountRow: settingsElements.stDevicesCountRow,
            stDevicesCount: settingsElements.stDevicesCount,
            stDevicesList: settingsElements.stDevicesList,
            stDevicesContainer: settingsElements.stDevicesContainer,
         });
         DawnSmartThings.setConfirmModal(showConfirmModal);
         DawnSmartThings.setCallbacks({
            getAuthState: callbacks.getAuthState,
         });

         // SmartThings button listeners
         if (settingsElements.stConnectBtn) {
            settingsElements.stConnectBtn.addEventListener('click', DawnSmartThings.startOAuth);
         }
         if (settingsElements.stRefreshBtn) {
            settingsElements.stRefreshBtn.addEventListener('click', DawnSmartThings.refreshDevices);
         }
         if (settingsElements.stDisconnectBtn) {
            settingsElements.stDisconnectBtn.addEventListener('click', DawnSmartThings.disconnect);
         }
      }
   }

   /* =============================================================================
    * Initialization
    * ============================================================================= */

   function init() {
      initElements();
      initConfirmModal();
      initInputModal();
      initListeners();
   }

   /**
    * Set callbacks for external dependencies
    */
   function setCallbacks(cbs) {
      if (cbs.getAuthState) callbacks.getAuthState = cbs.getAuthState;
      if (cbs.setAuthState) callbacks.setAuthState = cbs.setAuthState;
      if (cbs.updateHistoryButtonVisibility)
         callbacks.updateHistoryButtonVisibility = cbs.updateHistoryButtonVisibility;
      if (cbs.restoreHistorySidebarState)
         callbacks.restoreHistorySidebarState = cbs.restoreHistorySidebarState;
   }

   /**
    * Get SmartThings elements (for external modules)
    */
   function getSmartThingsElements() {
      return {
         stStatusIndicator: settingsElements.stStatusIndicator,
         stStatusText: settingsElements.stStatusText,
         stNotConfigured: settingsElements.stNotConfigured,
         stConnectBtn: settingsElements.stConnectBtn,
         stRefreshBtn: settingsElements.stRefreshBtn,
         stDisconnectBtn: settingsElements.stDisconnectBtn,
         stDevicesCountRow: settingsElements.stDevicesCountRow,
         stDevicesCount: settingsElements.stDevicesCount,
         stDevicesList: settingsElements.stDevicesList,
         stDevicesContainer: settingsElements.stDevicesContainer,
      };
   }

   /* =============================================================================
    * Export
    * ============================================================================= */

   window.DawnSettings = {
      init: init,
      setCallbacks: setCallbacks,
      open: open,
      close: close,
      openSection: openSection,
      requestConfig: requestConfig,
      // Response handlers
      handleGetConfigResponse: handleGetConfigResponse,
      handleSetConfigResponse: handleSetConfigResponse,
      handleSetSecretsResponse: handleSetSecretsResponse,
      handleModelsListResponse: handleModelsListResponse,
      handleInterfacesListResponse: handleInterfacesListResponse,
      handleRestartResponse: handleRestartResponse,
      handleGetAudioDevicesResponse: handleGetAudioDevicesResponse,
      handleSystemPromptResponse: handleSystemPromptResponse,
      handleSetSessionLlmResponse: handleSetSessionLlmResponse,
      handleListLlmModelsResponse: handleListLlmModelsResponse,
      // LLM controls
      updateLlmControls: updateLlmControls,
      updateCloudModelLists: updateCloudModelLists,
      // Per-conversation LLM settings
      initConversationLlmControls: initConversationLlmControls,
      resetConversationLlmControls: resetConversationLlmControls,
      applyConversationLlmSettings: applyConversationLlmSettings,
      lockConversationLlmSettings: lockConversationLlmSettings,
      getConversationLlmSettings: getConversationLlmSettings,
      isConversationLlmLocked: () => conversationLlmState.locked,
      // Modals (shared with other modules)
      showConfirmModal: showConfirmModal,
      showInputModal: showInputModal,
      trapFocus: trapFocus,
      // Auth visibility
      updateAuthVisibility: updateAuthVisibility,
      // SmartThings elements
      getSmartThingsElements: getSmartThingsElements,
   };
})();
