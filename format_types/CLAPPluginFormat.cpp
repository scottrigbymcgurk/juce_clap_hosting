#include "CLAPPluginFormat.h"

// JUCE 8 no longer bundles CoreAudioUtilityClasses/CAXException.h (that was a
// JUCE-7-era Apple header the fork pulled in only for CFBundleRef). In JUCE 8,
// CFBundleRef is available directly from <CoreFoundation/CoreFoundation.h> on
// macOS. juce_mac_CFHelpers.h was also renamed to juce_CFHelpers_mac.h in the
// JUCE-8 native-file renaming pass (S5 convention: platform suffix trails).
// Rule 3 build-fix per plan 03-00b deferred item "VSTL8TERS_CLAP_HOSTING_READY
// flip requires fixing the fork's JUCE-7-era include path". Additive only —
// no upstream symbol is renamed or removed.
#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h> // CFBundleRef
 #include "juce_core/native/juce_CFHelpers_mac.h" // CFUniquePtr (JUCE 8 filename)
#endif

#include <clap/clap.h>
#include <clap/helpers/event-list.hh>
#include <clap/helpers/reducing-param-queue.hh>
#include <clap/helpers/reducing-param-queue.hxx>

namespace juce {
    namespace juce_clap_helpers {
        template<typename Range>
        static int getHashForRange(Range &&range) noexcept {
            uint32 value = 0;

            for (const auto &item: range)
                value = (value * 31) + (uint32) item;

            return (int) value;
        }

        void fillDescription (PluginDescription &description, const clap_plugin_descriptor *clapDesc)
        {
            description.manufacturerName = clapDesc->vendor;;
            description.name = clapDesc->name;
            description.descriptiveName = clapDesc->description;
            description.pluginFormatName = "CLAP";

            description.uniqueId = getHashForRange(std::string(clapDesc->id));

            description.version = clapDesc->version;
            description.category = clapDesc->features[1] != nullptr ? clapDesc->features[1]
                                                                    : clapDesc->features[0]; // @TODO: better feature detection...

            description.isInstrument = clapDesc->features[0] == std::string(CLAP_PLUGIN_FEATURE_INSTRUMENT);
        }

        static void createPluginDescription(PluginDescription &description,
                                            const File &pluginFile, const clap_plugin_descriptor *clapDesc,
                                            int numInputs, int numOutputs)
        {
            description.fileOrIdentifier = pluginFile.getFullPathName();
            description.lastFileModTime = pluginFile.getLastModificationTime();
            description.lastInfoUpdateTime = Time::getCurrentTime();

            description.numInputChannels = numInputs;
            description.numOutputChannels = numOutputs;

            fillDescription (description, clapDesc);

        }

        //==============================================================================
        struct DescriptionFactory {
            explicit DescriptionFactory(const clap_plugin_factory *pluginFactory)
                    : factory(pluginFactory) {
                jassert (pluginFactory != nullptr);
            }

            virtual ~DescriptionFactory() = default;

            Result findDescriptionsAndPerform(const File &pluginFile) {
                Result result{Result::ok()};
                const auto count = factory->get_plugin_count(factory);

                for (uint32_t i = 0; i < count; ++i) {
                    const auto clapDesc = factory->get_plugin_descriptor(factory, i);
                    PluginDescription desc;

                    createPluginDescription(desc, pluginFile, clapDesc, 2, 2); // @TODO: get inputs and outputs...

                    result = performOnDescription(desc);

                    if (result.failed())
                        break;
                }

                return result;
            }

            virtual Result performOnDescription(PluginDescription &) = 0;

        private:
            const clap_plugin_factory *factory;

            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DescriptionFactory)
        };

        struct DescriptionLister : public DescriptionFactory {
            explicit DescriptionLister(const clap_plugin_factory *pluginFactory)
                    : DescriptionFactory(pluginFactory) {
            }

            Result performOnDescription(PluginDescription &desc) override {
                list.add(std::make_unique<PluginDescription>(desc));
                return Result::ok();
            }

            OwnedArray<PluginDescription> list;

        private:
            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DescriptionLister)
        };


        //==============================================================================
        struct DLLHandle {
            DLLHandle(const File &fileToOpen)
                    : dllFile(fileToOpen) {
                open();
                entry = reinterpret_cast<const clap_plugin_entry *> (getFunction("clap_entry"));
                jassert (entry != nullptr);

                entry->init(dllFile.getFullPathName().toStdString().c_str());
            }

            ~DLLHandle() {
#if JUCE_MAC
                if (bundleRef != nullptr)
#endif
                {
                    if (entry != nullptr)
                        entry->deinit();

//                using ExitModuleFn = bool (PLUGIN_API*) ();
//
//                if (auto* exitFn = (ExitModuleFn) getFunction (exitFnName))
//                    exitFn();

#if JUCE_WINDOWS || JUCE_LINUX || JUCE_BSD
                    library.close();
#endif
                }
            }

            //==============================================================================
            /** The factory should begin with a refCount of 1, so don't increment the reference count
                (ie: don't use a VSTComSmartPtr in here)! Its lifetime will be handled by this DLLHandle.
            */
            const clap_plugin_factory *JUCE_CALLTYPE getPluginFactory() {
                if (factory == nullptr)
                    factory = static_cast<const clap_plugin_factory *>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));

                // The plugin NEEDS to provide a factory to be able to be called a VST3!
                // Most likely you are trying to load a 32-bit VST3 from a 64-bit host
                // or vice versa.
                jassert (factory != nullptr);
                return factory;
            }

            void *getFunction(const char *functionName) {
#if JUCE_WINDOWS || JUCE_LINUX || JUCE_BSD
                return library.getFunction (functionName);
#elif JUCE_MAC
                if (bundleRef == nullptr)
                    return nullptr;

                CFUniquePtr<CFStringRef> name(String(functionName).toCFString());
                return CFBundleGetFunctionPointerForName(bundleRef.get(), name.get());
#endif
            }

            File getFile() const noexcept { return dllFile; }

        private:
            File dllFile;
            const clap_plugin_entry *entry = nullptr;
            const clap_plugin_factory *factory = nullptr;

#if JUCE_WINDOWS
            static constexpr const char* entryFnName = "clap_entry";

            using EntryProc = bool (PLUGIN_API*) ();
#elif JUCE_LINUX || JUCE_BSD
            static constexpr const char* entryFnName = "clap_entry";

            using EntryProc = bool (PLUGIN_API*) (void*);
#elif JUCE_MAC
            static constexpr const char *entryFnName = "clap_entry";

            using EntryProc = bool (*)(CFBundleRef);
#endif

            //==============================================================================
#if JUCE_WINDOWS || JUCE_LINUX || JUCE_BSD
            DynamicLibrary library;

        bool open()
        {
            if (library.open (dllFile.getFullPathName()))
                return true;

            return false;
        }
#elif JUCE_MAC
            CFUniquePtr<CFBundleRef> bundleRef;

            bool open() {
                auto *utf8 = dllFile.getFullPathName().toRawUTF8();

                if (auto url = CFUniquePtr<CFURLRef>(CFURLCreateFromFileSystemRepresentation(nullptr,
                                                                                             (const UInt8 *) utf8,
                                                                                             (CFIndex) std::strlen(
                                                                                                     utf8),
                                                                                             dllFile.isDirectory()))) {
                    bundleRef.reset(CFBundleCreate(kCFAllocatorDefault, url.get()));

                    if (bundleRef != nullptr) {
                        CFObjectHolder<CFErrorRef> error;

                        if (CFBundleLoadExecutableAndReturnError(bundleRef.get(), &error.object))
                            return true;

                        if (error.object != nullptr)
                            if (auto failureMessage = CFUniquePtr<CFStringRef>(CFErrorCopyFailureReason(error.object)))
                                DBG (String::fromCFString(failureMessage.get()));

                        bundleRef = nullptr;
                    }
                }

                return false;
            }

#endif

            //==============================================================================
            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DLLHandle)
        };

        struct DLLHandleCache : public DeletedAtShutdown {
            DLLHandleCache() = default;

            ~DLLHandleCache() override { clearSingletonInstance(); }

            JUCE_DECLARE_SINGLETON (DLLHandleCache, false)

            DLLHandle &findOrCreateHandle(const String &modulePath) {
#if JUCE_LINUX || JUCE_BSD
                File file (getDLLFileFromBundle (modulePath));
#else
                File file(modulePath);
#endif

                auto it = std::find_if(openHandles.begin(), openHandles.end(),
                                       [&](const std::unique_ptr<DLLHandle> &handle) {
                                           return file == handle->getFile();
                                       });

                if (it != openHandles.end())
                    return *it->get();

                openHandles.push_back(std::make_unique<DLLHandle>(file));
                return *openHandles.back().get();
            }

        private:
#if JUCE_LINUX || JUCE_BSD
            File getDLLFileFromBundle (const String& bundlePath) const
        {
            auto machineName = []() -> String
            {
                struct utsname unameData;
                auto res = uname (&unameData);

                if (res != 0)
                    return {};

                return unameData.machine;
            }();

            File file (bundlePath);

            return file.getChildFile ("Contents")
                       .getChildFile (machineName + "-linux")
                       .getChildFile (file.getFileNameWithoutExtension() + ".so");
        }
#endif

            std::vector<std::unique_ptr<DLLHandle>> openHandles;

            //==============================================================================
            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DLLHandleCache)
        };


        JUCE_IMPLEMENT_SINGLETON (DLLHandleCache)
    }

    //==============================================================================
    class CLAPPluginInstance final  : public AudioPluginInstance,
                                      private Timer
    {
        struct HostToPluginParamQueueValue {
            void *cookie;
            double value;
        };

        struct PluginToHostParamQueueValue {
            void update(const PluginToHostParamQueueValue& v) noexcept {
                if (v.has_value) {
                    has_value = true;
                    value = v.value;
                }

                if (v.has_gesture) {
                    has_gesture = true;
                    is_begin = v.is_begin;
                }
            }

            bool has_value = false;
            bool has_gesture = false;
            bool is_begin = false;
            float value = 0;
        };

        clap::helpers::ReducingParamQueue<clap_id, HostToPluginParamQueueValue> hostToPluginParamQueue;
        clap::helpers::ReducingParamQueue<clap_id, PluginToHostParamQueueValue> pluginToHostParamQueue;

    public:
        //==============================================================================
        struct  CLAPParameter final : public Parameter
        {
            CLAPParameter (clap_param_info pInfo,
                           const clap_plugin_params* pluginPs,
                           const clap_plugin* plug,
                           clap::helpers::ReducingParamQueue<clap_id, HostToPluginParamQueueValue>& paramQueue) : paramInfo (pInfo),
                                                      pluginParams (pluginPs),
                                                      plugin (plug)
            {
                createParamChangeEvent = [&paramQueue] (clap_id id, void* cookie, double val)
                {
                    paramQueue.set(id, {cookie, val});
                    paramQueue.producerDone();
                    // @TODO: request param flush here...
                };
            }

            float getValue() const override
            {
                double value {};
                pluginParams->get_value (plugin, paramInfo.id, &value);
                return (float) value;
            }

            void setValue (float newValue) override
            {
                createParamChangeEvent (paramInfo.id, paramInfo.cookie, (double) newValue);
            }

            /*  If we're syncing the editor to the processor, the processor won't need to
                be notified about the parameter updates, so we can avoid flagging the
                change when updating the float cache.
            */
            void setValueWithoutUpdatingProcessor (float newValue)
            {
                sendValueChangedMessageToListeners (newValue);
            }

            String getText (float value, int maxLength) const override
            {
                std::vector<char> display ((size_t) maxLength);

                pluginParams->value_to_text (plugin, paramInfo.id, (double) value, display.data(), (uint32_t) maxLength);

                return String { display.data(), (size_t) maxLength };
            }

            float getValueForText (const String& text) const override
            {
                double value {};
                pluginParams->text_to_value (plugin, paramInfo.id, text.toRawUTF8(), &value);
                return (float) value;
            }

            float getDefaultValue() const override
            {
                return (float) paramInfo.default_value;
            }

            String getName (int /*maximumStringLength*/) const override
            {
                return String { paramInfo.name };
            }

            String getLabel() const override
            {
                return {}; // @TODO
            }

            bool isAutomatable() const override
            {
                return paramInfo.flags & CLAP_PARAM_IS_AUTOMATABLE;
            }

            bool isDiscrete() const override
            {
                return paramInfo.flags & CLAP_PARAM_IS_STEPPED;
            }

            int getNumSteps() const override
            {
                return 100;  // @TODO
            }

            StringArray getAllValueStrings() const override
            {
                return {};
            }

            String getParameterID() const override
            {
                return String (paramInfo.id);
            }

            clap_param_info paramInfo;
            const clap_plugin_params* pluginParams;
            const clap_plugin* plugin;
            std::function<void (clap_id, void*, double)> createParamChangeEvent;
        };

    public:
        //==============================================================================
        explicit CLAPPluginInstance (const clap_plugin_factory* factory, const char* clapID, const File& file)
                : AudioPluginInstance (BusesProperties{}), pluginFile (file)
        {
            host.host_data = this;
            host.clap_version = CLAP_VERSION;
            host.name = "JUCE Clap Host";
            host.version = "0.0.1";
            host.vendor = "clap";
            host.url = "https://github.com/free-audio/clap";
            host.get_extension = CLAPPluginInstance::clapExtension;
            host.request_callback = CLAPPluginInstance::clapRequestCallback;
            host.request_process = CLAPPluginInstance::clapRequestProcess;
            host.request_restart = CLAPPluginInstance::clapRequestRestart;

            plugin = factory->create_plugin (factory, &host, clapID);
        }

        ~CLAPPluginInstance() override
        {
            cleanup();
        }

        void cleanup()
        {
            jassert (getActiveEditor() == nullptr); // You must delete any editors before deleting the plugin instance!

            releaseResources();

            plugin->destroy (plugin);
        }

        //==============================================================================
        bool initialise()
        {
            // The CLAP spec mandates that initialization is called on the main thread.
            JUCE_ASSERT_MESSAGE_THREAD

            if (plugin == nullptr)
                return false;

            if (! plugin->init (plugin))
                return false;

            initPluginExtension(pluginParams, CLAP_EXT_PARAMS);
            initPluginExtension(pluginAudioPorts, CLAP_EXT_AUDIO_PORTS);
            initPluginExtension(pluginGui, CLAP_EXT_GUI);
            initPluginExtension(pluginTimerSupport, CLAP_EXT_TIMER_SUPPORT);
            initPluginExtension(pluginPosixFdSupport, CLAP_EXT_POSIX_FD_SUPPORT);
            initPluginExtension(pluginState, CLAP_EXT_STATE);
            // Spike 001 priority-3 (vst-l8ters Plan 03-04): register
            // CLAP_EXT_NOTE_PORTS so hosted CLAP instruments can advertise
            // their note input ports. Precursor to IO-03 MIDI routing to
            // hosted CLAP instruments (MidiRouter in Plan 03-07 feeds
            // clap_event_note events through the eventsIn buffer; the
            // plugin needs this extension initialised for the routing to
            // have a declared destination).
            initPluginExtension(pluginNotePorts, CLAP_EXT_NOTE_PORTS);

            refreshParameterList();

            startTimerHz (10); // @TODO: what's a good value here?

            return true;
        }

//        void getExtensions (ExtensionsVisitor& visitor) const override
//        {
//            struct Extensions :  public ExtensionsVisitor::VST3Client,
//                                 public ExtensionsVisitor::ARAClient
//            {
//                explicit Extensions (const VST3PluginInstance* instanceIn) : instance (instanceIn) {}
//
//                Steinberg::Vst::IComponent* getIComponentPtr() const noexcept override   { return instance->holder->component; }
//
//                MemoryBlock getPreset() const override             { return instance->getStateForPresetFile(); }
//
//                bool setPreset (const MemoryBlock& rawData) const override
//                {
//                    return instance->setStateFromPresetFile (rawData);
//                }
//
//                void createARAFactoryAsync (std::function<void (ARAFactoryWrapper)> cb) const noexcept override
//                {
//                    cb (ARAFactoryWrapper { ::juce::getARAFactory (*(instance->holder->module)) });
//                }
//
//                const VST3PluginInstance* instance = nullptr;
//            };
//
//            Extensions extensions { this };
//            visitor.visitVST3Client (extensions);
//
//            if (::juce::getARAFactory (*(holder->module)))
//            {
//                visitor.visitARAClient (extensions);
//            }
//        }
//
//        void* getPlatformSpecificData() override   { return holder->component; }

        //==============================================================================
        const String getName() const override
        {
            return plugin->desc->name;
        }

        void prepareToPlay (double newSampleRate, int estimatedSamplesPerBlock) override
        {
            // The VST3 spec requires that IComponent::setupProcessing() is called on the message
            // thread. If you call it from a different thread, some plugins may break.
            JUCE_ASSERT_MESSAGE_THREAD
            MessageManagerLock lock;

            setRateAndBufferSizeDetails (newSampleRate, estimatedSamplesPerBlock);
            plugin->activate (plugin, newSampleRate, 1, (uint32_t) estimatedSamplesPerBlock);
            isPluginActive = true;
            plugin->start_processing (plugin);
        }

        void releaseResources() override
        {
            plugin->stop_processing (plugin);
            plugin->deactivate (plugin);
            isPluginActive = false;
        }

//        bool supportsDoublePrecisionProcessing() const override
//        {
//            return (processor->canProcessSampleSize (Vst::kSample64) == kResultTrue);
//        }

        //==============================================================================
        void processBlock (AudioBuffer<float>& buffer, MidiBuffer& midiMessages) override
        {
            jassert (! isUsingDoublePrecision());

//            const SpinLock::ScopedLockType processLock (processMutex); // I don't want to do this, but I will if I have to...

            // @TODO: check for active state...
            clap_audio_buffer clapBuffer {};
            clapBuffer.channel_count = (uint32_t) buffer.getNumChannels();
            clapBuffer.constant_mask = 0;
            // JUCE 8 narrowed getArrayOfWritePointers to return `float* const*`
            // (const pointer-to-pointer) where JUCE 7 returned `float**`. The
            // CLAP C ABI's clap_audio_buffer::data32 is `float**`. We cast
            // back to the mutable form — safe because JUCE's const-narrowing
            // applies to the pointer-array itself, not the underlying
            // channel-data buffers (CLAP is allowed to write into the
            // per-channel buffers; it is NOT allowed to swap the per-channel
            // pointers, which matches JUCE's invariant). [Rule 3 build-fix
            // for JUCE 7 -> 8 drift — same category as the CFBundleRef
            // include rename in Task 01.]
            clapBuffer.data32 = const_cast<float**> (buffer.getArrayOfWritePointers());
            clapBuffer.data64 = nullptr;
            clapBuffer.latency = 0; // @TODO

            clap_process processState {};
            processState.in_events = eventsIn.clapInputEvents();
            processState.out_events = eventsOut.clapOutputEvents();
            processState.frames_count = (uint32_t) buffer.getNumSamples();
            processState.steady_time = -1; // @TODO
            processState.transport = nullptr; // @TODO

            processState.audio_inputs = &clapBuffer;
            processState.audio_inputs_count = 1;
            processState.audio_outputs = &clapBuffer;
            processState.audio_outputs_count = 1;

            eventsOut.clear();
            generatePluginInputEvents();

            // Spike 001 priority-3 (vst-l8ters Plan 03-04): translate the
            // incoming juce::MidiBuffer into CLAP note / midi events and push
            // them into the plugin's input-event buffer for this block.
            //
            // Contract:
            //   - NoteOn  -> CLAP_EVENT_NOTE_ON with velocity scaled to 0..1
            //                (MIDI is 0..127; CLAP is 0..1 double).
            //   - NoteOff -> CLAP_EVENT_NOTE_OFF with release velocity scaled.
            //   - Other   -> CLAP_EVENT_MIDI with raw 3-byte payload
            //                (pitchbend, CC, program change, aftertouch, etc.).
            //
            // Wildcard note addressing: port=0 / note_id=-1 for host-originated
            // MIDI (no note-id tracking on the host side in v1). Channel + key
            // come from the MIDI message itself. Matches CLAP's PCKN convention
            // (clap/events.h lines 124-149).
            //
            // RT-safety (CLAUDE.md Invariant #1): the loop allocates only on
            // the stack (POD event structs); juce::MidiBuffer::getNumEvents()
            // and MidiBufferIterator are RT-safe; eventsIn.push() is a
            // contiguous write into clap::helpers::EventList's arena. No
            // audio-thread heap allocations or locks.
            for (const auto meta : midiMessages)
            {
                const auto& m = meta.getMessage();
                const uint32_t sampleOffset = (uint32_t) juce::jmax (0, meta.samplePosition);

                if (m.isNoteOn())
                {
                    clap_event_note ev {};
                    ev.header.size     = sizeof (ev);
                    ev.header.time     = sampleOffset;
                    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                    ev.header.type     = CLAP_EVENT_NOTE_ON;
                    ev.header.flags    = CLAP_EVENT_IS_LIVE;
                    ev.note_id    = -1;  // host-originated; no note-id tracking in v1
                    ev.port_index = 0;   // single host note-input port
                    ev.channel    = (int16_t) juce::jmax (0, m.getChannel() - 1);  // JUCE 1..16 -> CLAP 0..15
                    ev.key        = (int16_t) m.getNoteNumber();
                    ev.velocity   = (double) m.getFloatVelocity();
                    eventsIn.push (&ev.header);
                }
                else if (m.isNoteOff())
                {
                    clap_event_note ev {};
                    ev.header.size     = sizeof (ev);
                    ev.header.time     = sampleOffset;
                    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                    ev.header.type     = CLAP_EVENT_NOTE_OFF;
                    ev.header.flags    = CLAP_EVENT_IS_LIVE;
                    ev.note_id    = -1;
                    ev.port_index = 0;
                    ev.channel    = (int16_t) juce::jmax (0, m.getChannel() - 1);
                    ev.key        = (int16_t) m.getNoteNumber();
                    ev.velocity   = (double) m.getFloatVelocity();  // release velocity
                    eventsIn.push (&ev.header);
                }
                else
                {
                    // Non-note MIDI (CC, pitch-bend, program change, aftertouch,
                    // channel pressure). Forward as raw CLAP_EVENT_MIDI.
                    // SysEx (>3 bytes) out-of-scope for priority-3 (see
                    // CLAP_EVENT_MIDI_SYSEX acknowledgement in the output
                    // switch above; full sysex routing is Plan 03-07 / IO-03).
                    if (m.getRawDataSize() > 3)
                        continue;
                    clap_event_midi ev {};
                    ev.header.size     = sizeof (ev);
                    ev.header.time     = sampleOffset;
                    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                    ev.header.type     = CLAP_EVENT_MIDI;
                    ev.header.flags    = CLAP_EVENT_IS_LIVE;
                    ev.port_index = 0;
                    const auto* raw = m.getRawData();
                    const int   n   = m.getRawDataSize();
                    ev.data[0] = (n > 0) ? raw[0] : (uint8_t) 0;
                    ev.data[1] = (n > 1) ? raw[1] : (uint8_t) 0;
                    ev.data[2] = (n > 2) ? raw[2] : (uint8_t) 0;
                    eventsIn.push (&ev.header);
                }
            }

            const auto status = plugin->process (plugin, &processState);
            ignoreUnused (status); // @TODO: figure out what to do with status

            handlePluginOutputEvents();

            eventsIn.clear();
            eventsOut.clear();

            pluginToHostParamQueue.producerDone();
        }

        void generatePluginInputEvents() {
            hostToPluginParamQueue.consume (
                    [this](clap_id param_id, const HostToPluginParamQueueValue& value) {
                        clap_event_param_value ev {};
                        ev.header.time = 0;
                        ev.header.type = CLAP_EVENT_PARAM_VALUE;
                        ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                        ev.header.flags = 0;
                        ev.header.size = sizeof(ev);
                        ev.param_id = param_id;
                        ev.cookie = value.cookie;
                        ev.port_index = 0;
                        ev.key = -1;
                        ev.channel = -1;
                        ev.note_id = -1;
                        ev.value = value.value;
                        eventsIn.push (&ev.header);
                    });
        }

        void handlePluginOutputEvents()
        {
            for (uint32_t i = 0; i < eventsOut.size(); ++i) {
                auto h = eventsOut.get(i);
                switch (h->type) {
                    case CLAP_EVENT_PARAM_GESTURE_BEGIN: {
                        auto ev = reinterpret_cast<const clap_event_param_gesture *>(h);

                        PluginToHostParamQueueValue v;
                        v.has_gesture = true;
                        v.is_begin = true;
                        pluginToHostParamQueue.setOrUpdate(ev->param_id, v);
                        break;
                    }

                    case CLAP_EVENT_PARAM_GESTURE_END: {
                        auto ev = reinterpret_cast<const clap_event_param_gesture *>(h);

                        PluginToHostParamQueueValue v;
                        v.has_gesture = true;
                        v.is_begin = false;
                        pluginToHostParamQueue.setOrUpdate(ev->param_id, v);
                        break;
                    }

                    case CLAP_EVENT_PARAM_VALUE: {
                        auto ev = reinterpret_cast<const clap_event_param_value *>(h);
                        PluginToHostParamQueueValue v;
                        v.has_value = true;
                        v.value = (float) ev->value;
                        pluginToHostParamQueue.setOrUpdate(ev->param_id, v);
                        break;
                    }

                    // Spike 001 priority-3 (vst-l8ters Plan 03-04, ~150 LOC budget):
                    // NOTE_ON / NOTE_OFF / NOTE_CHOKE / NOTE_END event cases.
                    // The plugin emits these to tell the host "voice started"
                    // (NOTE_ON echo — rare; most plugins only echo NOTE_END),
                    // "voice ended" (NOTE_END — CLAP's cooperative voice-life
                    // matching per clap/events.h lines 65-88), or "choke this
                    // voice" (NOTE_CHOKE). We acknowledge the type to prevent
                    // the default-drop, then log in JUCE_DEBUG for now. Real
                    // voice-tracking wiring lands when VST L8ters implements
                    // polyphonic modulation (v1.x — explicitly out of v1 per
                    // PROJECT.md "Polyphonic modulation of hosted-plugin
                    // parameters (physically impossible)" — but the SPEC-COMPLIANT
                    // acknowledgement must be here from day one so the plugin
                    // doesn't experience a silent drop).
                    case CLAP_EVENT_NOTE_ON:
                    case CLAP_EVENT_NOTE_OFF:
                    case CLAP_EVENT_NOTE_CHOKE:
                    case CLAP_EVENT_NOTE_END: {
                       #if JUCE_DEBUG
                        auto ev = reinterpret_cast<const clap_event_note*>(h);
                        const char* kind = (h->type == CLAP_EVENT_NOTE_ON)    ? "NOTE_ON"
                                         : (h->type == CLAP_EVENT_NOTE_OFF)   ? "NOTE_OFF"
                                         : (h->type == CLAP_EVENT_NOTE_CHOKE) ? "NOTE_CHOKE"
                                                                              : "NOTE_END";
                        Logger::writeToLog (String ("[clap-host] ") + kind
                                            + " key=" + String ((int) ev->key)
                                            + " ch="  + String ((int) ev->channel)
                                            + " vel=" + String (ev->velocity));
                       #else
                        (void) h;
                       #endif
                        break;
                    }

                    // Spike 001 priority-3: raw MIDI event echo from the plugin.
                    // Plugins that implement clap_plugin_note_ports with
                    // CLAP_NOTE_DIALECT_MIDI as an OUTPUT dialect emit these.
                    // Acknowledge to avoid default-drop; forwarding MIDI out to
                    // the DAW is IO-03's concern (Plan 03-07 wires the
                    // juce::MidiBuffer -> DAW midiMessages pass-through).
                    case CLAP_EVENT_MIDI: {
                       #if JUCE_DEBUG
                        auto ev = reinterpret_cast<const clap_event_midi*>(h);
                        Logger::writeToLog (String ("[clap-host] MIDI raw=")
                                            + String::toHexString ((int) ev->data[0]) + ","
                                            + String::toHexString ((int) ev->data[1]) + ","
                                            + String::toHexString ((int) ev->data[2]));
                       #else
                        (void) h;
                       #endif
                        break;
                    }

                    case CLAP_EVENT_MIDI_SYSEX:
                    case CLAP_EVENT_MIDI2: {
                        // Acknowledge to prevent default-drop. MIDI-sysex +
                        // MIDI2 full forwarding is IO-03 scope (Plan 03-07).
                        (void) h;
                        break;
                    }

                    // Spike 001 priority-3: TRANSPORT event. Hosts normally
                    // SEND transport info to the plugin (via clap_process.transport);
                    // plugins MAY echo transport events on their output stream
                    // to signal tempo-sensitive internal state changes. IO-05
                    // (Plan 03-07) wires the AudioPlayHead::PositionInfo -> CLAP
                    // transport translation on the INPUT side. Acknowledge on
                    // the OUTPUT side here for spec completeness.
                    case CLAP_EVENT_TRANSPORT: {
                       #if JUCE_DEBUG
                        auto ev = reinterpret_cast<const clap_event_transport*>(h);
                        Logger::writeToLog (String ("[clap-host] TRANSPORT tempo=")
                                            + String (ev->tempo)
                                            + " flags=" + String ((int) ev->flags));
                       #else
                        (void) h;
                       #endif
                        break;
                    }

                    case CLAP_EVENT_PARAM_MOD: {
                        // Spike 001 priority-1 (~15 LOC): acknowledge
                        // CLAP_EVENT_PARAM_MOD events coming out of the plugin so
                        // the switch does not drop them into the implicit default
                        // (silent drop).
                        //
                        // This is the D-15 NON-DESTRUCTIVE path — the event
                        // indicates the plugin has observed a modulation amount
                        // and is reporting it back (bypass/echo pattern). We do
                        // NOT push this into pluginToHostParamQueue because
                        // pluginToHostParamQueue is the destructive-value mirror
                        // (AudioProcessorParameter::setValueWithoutUpdatingProcessor
                        // path in timerCallback). Per the CLAP spec comment in
                        // clap/events.h line 103-104, "The value heard is:
                        // param_value + param_mod" — param_mod is a
                        // superimposed offset, not a value replacement. Leaking
                        // it into the JUCE parameter state would bake
                        // modulation into the authoritative value, which is the
                        // opposite of non-destructive.
                        //
                        // Acknowledging the case here is the minimum
                        // additive change to stop the default drop. Host
                        // -> plugin PARAM_MOD emission happens from VST
                        // L8ters's own ClapModulationEmitter (our-side
                        // glue; see Source/Hosting/ClapModulationEmitter.cpp)
                        // and is fed into the plugin through eventsIn (the
                        // processBlock-side input queue), independent of
                        // this output-event switch.
                       #if JUCE_DEBUG
                        auto ev = reinterpret_cast<const clap_event_param_mod *>(h);
                        Logger::writeToLog ("[clap-host] PARAM_MOD param_id="
                                            + String ((int64) ev->param_id)
                                            + " amount=" + String (ev->amount));
                       #else
                        (void) h;
                       #endif
                        break;
                    }
                }
            }
        }

        void timerCallback() override
        {
            // Try to send events to the audio engine
            hostToPluginParamQueue.producerDone();

            pluginToHostParamQueue.consume(
                    [this](clap_id param_id, const PluginToHostParamQueueValue &value) {
                        auto it = paramMap.find(param_id);
                        if (it == paramMap.end()) {
                            // Plugin produced a CLAP_EVENT_PARAM_SET with an unknown param_id
                            jassertfalse;
                            return;
                        }

                        if (value.has_value)
                            it->second->setValueWithoutUpdatingProcessor (value.value);

                        if (value.has_gesture)
                        {
                            if (value.is_begin)
                                it->second->beginChangeGesture();
                            else
                                it->second->endChangeGesture();
                        }
                    });

            // @TODO: run previously schedlued param flush

            if (scheduleMainThreadCallback.compareAndSetBool (false, true))
                plugin->on_main_thread (plugin);

            if (scheduleRestart.compareAndSetBool (false, true))
            {
                if (isPluginActive)
                    releaseResources();
                prepareToPlay (getSampleRate(), getBlockSize());
            }
        }

        void processBlock (AudioBuffer<double>& buffer, MidiBuffer& midiMessages) override
        {
//            jassert (isUsingDoublePrecision());
//
//            const SpinLock::ScopedLockType processLock (processMutex);
//
//            if (isActive && processor != nullptr)
//                processAudio (buffer, midiMessages, Vst::kSample64, false);
        }

//        void processBlockBypassed (AudioBuffer<float>& buffer, MidiBuffer& midiMessages) override
//        {
//            jassert (! isUsingDoublePrecision());
//
//            const SpinLock::ScopedLockType processLock (processMutex);
//
//            if (bypassParam != nullptr)
//            {
//                if (isActive && processor != nullptr)
//                    processAudio (buffer, midiMessages, Vst::kSample32, true);
//            }
//            else
//            {
//                AudioProcessor::processBlockBypassed (buffer, midiMessages);
//            }
//        }
//
//        void processBlockBypassed (AudioBuffer<double>& buffer, MidiBuffer& midiMessages) override
//        {
//            jassert (isUsingDoublePrecision());
//
//            const SpinLock::ScopedLockType processLock (processMutex);
//
//            if (bypassParam != nullptr)
//            {
//                if (isActive && processor != nullptr)
//                    processAudio (buffer, midiMessages, Vst::kSample64, true);
//            }
//            else
//            {
//                AudioProcessor::processBlockBypassed (buffer, midiMessages);
//            }
//        }

        //==============================================================================
        bool canAddBus (bool) const override                                       { return false; }
        bool canRemoveBus (bool) const override                                    { return false; }

        bool isBusesLayoutSupported (const BusesLayout& layouts) const override
        {
//            const SpinLock::ScopedLockType processLock (processMutex);
//
//            // if the processor is not active, we ask the underlying plug-in if the
//            // layout is actually supported
//            if (! isActive)
//                return canApplyBusesLayout (layouts);
//
//            // not much we can do to check the layout while the audio processor is running
//            // Let's at least check if it is a VST3 compatible layout
//            for (int dir = 0; dir < 2; ++dir)
//            {
//                bool isInput = (dir == 0);
//                auto n = getBusCount (isInput);
//
//                for (int i = 0; i < n; ++i)
//                    if (getChannelLayoutOfBus (isInput, i).isDiscreteLayout())
//                        return false;
//            }

            return true;
        }

        bool canApplyBusesLayout (const BusesLayout& layouts) const override
        {
//            // someone tried to change the layout while the AudioProcessor is running
//            // call releaseResources first!
//            jassert (! isActive);
//
//            const auto previousLayout = getBusesLayout();
//            const auto result = syncBusLayouts (layouts);
//            syncBusLayouts (previousLayout);
//            return result;

            return false;
        }

        // Spike 001 priority-3 (vst-l8ters Plan 03-04): report MIDI I/O based
        // on the plugin's note_ports.count() result. Plugins that implement
        // clap_plugin_note_ports with at least one input port accept MIDI;
        // plugins with at least one output port produce MIDI. This is what
        // the JUCE wrapper-side (DAW) uses to drive "is this plugin wired to
        // MIDI?" affordances — without this, hosted CLAP instruments look
        // like audio-only effects to the DAW.
        bool acceptsMidi() const override
        {
            if (pluginNotePorts == nullptr || pluginNotePorts->count == nullptr)
                return false;
            return pluginNotePorts->count (plugin, /*is_input=*/true) > 0;
        }
        bool producesMidi() const override
        {
            if (pluginNotePorts == nullptr || pluginNotePorts->count == nullptr)
                return false;
            return pluginNotePorts->count (plugin, /*is_input=*/false) > 0;
        }

        //==============================================================================
        AudioProcessorParameter* getBypassParameter() const override         { return nullptr; }

        //==============================================================================
        /** May return a negative value as a means of informing us that the plugin has "infinite tail," or 0 for "no tail." */
        double getTailLengthSeconds() const override
        {
//            if (processor != nullptr)
//            {
//                auto sampleRate = getSampleRate();
//
//                if (sampleRate > 0.0)
//                {
//                    auto tailSamples = processor->getTailSamples();
//
//                    if (tailSamples == Vst::kInfiniteTail)
//                        return std::numeric_limits<double>::infinity();
//
//                    return jlimit (0, 0x7fffffff, (int) processor->getTailSamples()) / sampleRate;
//                }
//            }

            return 0.0;
        }

        //==============================================================================
        // CLAP GUI host-side helpers — platform API + window-handle marshalling.
        //
        // Spike 001 priority-2 (~240 LOC budget) + spike 003 Pattern 1
        // (probe-before-commit). Adds the full GUI lifecycle
        // (is_api_supported → create → set_parent → get_size → show → destroy)
        // on top of the existing pluginGui handle fetched in initialise()
        // (line ~470). The fork previously returned a GenericAudioProcessorEditor
        // DummyEditor; this replaces it with an embedded-plugin-native editor
        // that delegates layout + lifetime to the CLAP plugin itself.
        //
        // Reference pedigree:
        //   - clap/ext/gui.h lines 19-33 (lifecycle order) + lines 47-65 (API
        //     constants) + lines 71-84 (clap_window + union fields per platform)
        //   - spike 003 gui-probe.cpp lines 90-186 (probe handshake translated
        //     verbatim; set_parent + show added for the "expensive version")
        //   - Landmine 1 (Cardinal advertise-then-reject): probe-before-commit
        //     handshake catches create() refusal gracefully — caller sees a
        //     nullptr return and shows a disabled "Open window" UI affordance.
        //   - macOS: CLAP_WINDOW_API_COCOA expects void* cocoa = NSView*.
        //     JUCE's NSViewComponent is the embedding container; its
        //     getWindowHandle() returns the underlying NSView* once the
        //     component is peered (i.e. addToDesktop-ed). We use the AudioProcessor-
        //     Editor's own peer handle (Component::getWindowHandle()) because
        //     the editor IS the embedding parent (CLAP plugins are responsible
        //     for creating their own NSView inside the parent we hand them via
        //     set_parent). This matches the clap-host-demo pattern on macOS.
        static const char* getCurrentClapGuiApi()
        {
#if JUCE_LINUX
            return CLAP_WINDOW_API_X11;
#elif JUCE_WINDOWS
            return CLAP_WINDOW_API_WIN32;
#elif JUCE_MAC
            return CLAP_WINDOW_API_COCOA;
#else
#   error "unsupported platform"
#endif
        }

        static clap_window makeClapWindow (Component& window)
        {
            clap_window w;
#if JUCE_LINUX
            w.api = CLAP_WINDOW_API_X11;
            w.x11 = reinterpret_cast<clap_xwnd> (window.getWindowHandle());
#elif JUCE_MAC
            w.api = CLAP_WINDOW_API_COCOA;
            w.cocoa = reinterpret_cast<clap_nsview> (window.getWindowHandle());
#elif JUCE_WINDOWS
            w.api = CLAP_WINDOW_API_WIN32;
            w.win32 = reinterpret_cast<clap_hwnd> (window.getWindowHandle());
#endif
            return w;
        }

        //==============================================================================
        // Probe-before-commit handshake (spike 003 Pattern 1).
        //
        // Walks the GUI extension's API surface far enough to confirm the
        // plugin's create() will not refuse after advertising support
        // (Cardinal-style advertise-then-reject). Runs create() + destroy()
        // during probing — the CLAP spec allows create/destroy pairs; see
        // clap/ext/gui.h lines 117-134 where create() is documented as
        // allocating resources and destroy() as freeing them. The probe is
        // thus a full round-trip, not a cheap capability check.
        //
        // Returns true if the plugin is ready to embed; false otherwise
        // (caller returns nullptr from createEditor, and the rack UI shows
        // a disabled "Open window" affordance per D-12 graceful-degrade).
        bool probeGuiCapability()
        {
            if (pluginGui == nullptr)
                return false;

            const char* api = getCurrentClapGuiApi();
            if (! pluginGui->is_api_supported (plugin, api, /*is_floating=*/false))
                return false;

            // create() is the load-bearing check (Landmine 1 Cardinal defence).
            if (! pluginGui->create (plugin, api, /*is_floating=*/false))
                return false;

            // get_size is informational here (probe phase); it's re-read at
            // commit time in createEditor(). An invalid size (0 or failure)
            // after a successful create() is a pathological plugin — refuse.
            uint32_t w = 0, h = 0;
            const bool gotSize = pluginGui->get_size (plugin, &w, &h);
            pluginGui->destroy (plugin);
            return gotSize && w > 0 && h > 0;
        }

        AudioProcessorEditor* createEditor() override
        {
            if (pluginGui == nullptr)
                return nullptr;

            // Probe-before-commit: Cardinal-class plugins advertise support
            // but refuse create(). Refusing to return a nullptr editor here
            // lets the rack UI show a graceful "GUI unavailable" state
            // instead of a live embedded view that fails to render.
            if (! probeGuiCapability())
                return nullptr;

            // -----------------------------------------------------------------
            // Embedded CLAP plugin editor. The plugin creates its own native
            // view and parents it into OUR AudioProcessorEditor's window
            // peer (Component::getWindowHandle()). The editor owns the CLAP
            // create/destroy lifecycle; its dtor calls destroy() so the
            // plugin's native resources are freed at the right moment.
            // -----------------------------------------------------------------
            struct EmbeddedClapEditor : public AudioProcessorEditor,
                                        private ComponentMovementWatcher
            {
                EmbeddedClapEditor (CLAPPluginInstance& owner)
                    : AudioProcessorEditor (owner)
                    , ComponentMovementWatcher (this)
                    , pluginOwner (owner)
                    , gui (owner.pluginGui)
                    , plugin (owner.plugin)
                {
                    setOpaque (true);
                    // Addressable on Desktop is a prerequisite for getWindowHandle()
                    // returning a real native handle; AudioProcessorEditor
                    // lifecycle guarantees this via its host-side hosting
                    // chain (RackChain / HostedPluginSlot in VST L8ters).

                    // We re-create the GUI NOW — the probe's destroy() freed
                    // its resources. create() must be called again before
                    // set_parent/get_size/show.
                    if (! gui->create (plugin, getCurrentClapGuiApi(), false))
                    {
                        guiCreated = false;
                        return;
                    }
                    guiCreated = true;

                    // Initial size from plugin.
                    uint32_t w = 0, h = 0;
                    if (gui->get_size (plugin, &w, &h) && w > 0 && h > 0)
                        setSize ((int) w, (int) h);
                    else
                        setSize (400, 300); // conservative fallback

                    // Parent: the CLAP plugin embeds its own native view
                    // inside OUR window handle. set_parent is called AFTER
                    // setSize + (eventual) addToDesktop; we trigger the
                    // actual attach when the component becomes peered
                    // (componentPeerChanged / parentHierarchyChanged).
                }

                ~EmbeddedClapEditor() override
                {
                    if (guiCreated && gui != nullptr && plugin != nullptr)
                        gui->destroy (plugin);
                    pluginOwner.editorBeingDeleted (this);
                }

                void paint (Graphics& g) override
                {
                    // The plugin paints over our backing view; fill with a
                    // neutral background so JUCE's accessibility layer sees
                    // a painted component even before the CLAP view attaches.
                    g.fillAll (Colours::black);
                }

                // ComponentMovementWatcher hooks — set_parent once peered,
                // and forward resizes to the plugin.
                void componentMovedOrResized (bool /*wasMoved*/, bool wasResized) override
                {
                    if (wasResized && guiCreated && gui != nullptr && gui->set_size != nullptr)
                        gui->set_size (plugin, (uint32_t) getWidth(), (uint32_t) getHeight());
                }

                void componentPeerChanged() override
                {
                    if (! guiCreated || gui == nullptr)
                        return;
                    if (getPeer() == nullptr)
                        return; // component detached — plugin stays alive for reattach

                    if (! parentAttached)
                    {
                        clap_window w = makeClapWindow (*this);
                        if (gui->set_parent (plugin, &w))
                        {
                            parentAttached = true;
                            if (gui->show != nullptr)
                                gui->show (plugin);
                        }
                    }
                }

                void componentVisibilityChanged() override {}

                CLAPPluginInstance&      pluginOwner;
                const clap_plugin_gui*   gui { nullptr };
                const clap_plugin*       plugin { nullptr };
                bool                     guiCreated { false };
                bool                     parentAttached { false };

                JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EmbeddedClapEditor)
            };

            return new EmbeddedClapEditor (*this);
        }

        bool hasEditor() const override
        {
            // Advertise hasEditor() based on pluginGui handle presence.
            // The probe runs at createEditor() time (not here) because
            // is_api_supported is cheap but create() is not. Hosts that
            // call hasEditor() repeatedly (some do) get the cheap check;
            // the load-bearing handshake only fires when the user actually
            // requests the editor.
            return pluginGui != nullptr;
        }

        //==============================================================================
        int getNumPrograms() override                        { return 1; /* programNames.size(); */ }
        const String getProgramName (int index) override     { return {}; /* index >= 0 ? programNames[index] : String(); */ }
        void changeProgramName (int, const String&) override {}

        int getCurrentProgram() override
        {
//            if (programNames.size() > 0 && editController != nullptr)
//                if (auto* param = getParameterForID (programParameterID))
//                    return jmax (0, roundToInt (param->getValue() * (float) (programNames.size() - 1)));

            return 0;
        }

        void setCurrentProgram (int program) override
        {
//            if (programNames.size() > 0 && editController != nullptr)
//            {
//                auto value = static_cast<Vst::ParamValue> (program) / static_cast<Vst::ParamValue> (jmax (1, programNames.size() - 1));
//
//                if (auto* param = getParameterForID (programParameterID))
//                    param->setValueNotifyingHost ((float) value);
//            }
        }

        //==============================================================================
        void getStateInformation (MemoryBlock& destData) override
        {
            struct OutputStream {
                MemoryBlock& data;

                explicit OutputStream (MemoryBlock& d) : data (d) {
                    data.reset();
                }

                static int64_t write (const clap_ostream *stream, const void *buffer, uint64_t size)
                {
                    auto os = static_cast <const OutputStream*> (stream->ctx);
                    jassert (os != nullptr);
                    os->data.append (buffer, size);
                    return (int64_t) size;
                }
            };

            if (pluginState == nullptr)
                return; // plugin does not support state!

            OutputStream os { destData };
            clap_ostream clapStream {
                &os,
                &OutputStream::write,
            };
            pluginState->save (plugin, &clapStream);
        }

        void setStateInformation (const void* data, int sizeInBytes) override
        {
            if (pluginState == nullptr)
                return; // plugin does not support state!

            struct InputStream {
                MemoryBlock data;
                int bytesCount = 0;

                explicit InputStream (const void* d, int size) : data (d,  (size_t) size) {}

                static int64_t read (const clap_istream *stream, void *buffer, uint64_t size)
                {
                    auto is = static_cast <InputStream*> (stream->ctx);
                    jassert (is != nullptr);

                    int bytesToRead = jmin ((int) size, (int) is->data.getSize() - is->bytesCount);
                    if (bytesToRead <= 0)
                        return 0;

                    is->data.copyTo (buffer, is->bytesCount, (size_t) bytesToRead);
                    is->bytesCount += bytesToRead;
                    return bytesToRead;
                }
            };

            InputStream is { data, sizeInBytes };
            clap_istream clapStream {
                    &is,
                    &InputStream::read,
            };
            pluginState->load (plugin, &clapStream);
        }

        //==============================================================================
        void fillInPluginDescription (PluginDescription& description) const override
        {
            juce_clap_helpers::createPluginDescription (description, pluginFile, plugin->desc, getMainBusNumInputChannels(), getMainBusNumOutputChannels());
        }

//        /** @note Not applicable to VST3 */
//        void getCurrentProgramStateInformation (MemoryBlock& destData) override
//        {
//            destData.setSize (0, true);
//        }
//
//        /** @note Not applicable to VST3 */
//        void setCurrentProgramStateInformation (const void* data, int sizeInBytes) override
//        {
//            ignoreUnused (data, sizeInBytes);
//        }

    public:
        //==============================================================================
        // Spike 001 priority-3 (vst-l8ters Plan 03-04): public accessor for the
        // plugin's input-event buffer. Unlocks the end-to-end TestCLAP PARAM_MOD
        // audio roundtrip — ClapModulationEmitter (Source/Hosting/
        // ClapModulationEmitter.cpp, Plan 03-03) pushes CLAP_EVENT_PARAM_MOD
        // events INTO eventsIn via its clap_output_events_t* parameter. Without
        // a public accessor, emitter callers had no way to hand the fork's
        // eventsIn to emit() from OUR-side audio-thread code.
        //
        // Caller discipline:
        //   - Audio thread ONLY. The underlying clap::helpers::EventList is
        //     not thread-safe; the processBlock callback is the only valid
        //     producer.
        //   - Push BEFORE plugin->process runs (generatePluginInputEvents +
        //     the MidiBuffer translation loop are the fork's existing producers;
        //     add your events in the same window).
        //   - eventsIn is cleared at the END of processBlock; events added
        //     outside processBlock are silently discarded.
        clap::helpers::EventList& getInputEventsList() noexcept { return eventsIn; }
        const clap::helpers::EventList& getInputEventsList() const noexcept { return eventsIn; }

    private:
        clap_host host {};
        const clap_plugin *plugin = nullptr;
        const clap_plugin_params *pluginParams = nullptr;
        const clap_plugin_audio_ports *pluginAudioPorts = nullptr;
        const clap_plugin_gui *pluginGui = nullptr;
        const clap_plugin_timer_support *pluginTimerSupport = nullptr;
        const clap_plugin_posix_fd_support *pluginPosixFdSupport = nullptr;
        const clap_plugin_state *pluginState = nullptr;
        // Spike 001 priority-3 (vst-l8ters Plan 03-04): note-ports extension
        // handle. Advertise-only — we don't currently query port count / info
        // at host init time (the UI / IO-03 MidiRouter layer does that when it
        // needs to surface a plugin's note-input ports to the user).
        const clap_plugin_note_ports *pluginNotePorts = nullptr;

        bool isPluginActive { false };
        clap::helpers::EventList eventsIn;
        clap::helpers::EventList eventsOut;

        const File pluginFile;

        Atomic<bool> scheduleMainThreadCallback { false };
        Atomic<bool> scheduleRestart { false };

        static CLAPPluginInstance *fromHost(const clap_host *host)
        {
            jassert (host != nullptr);

            auto* h = static_cast<CLAPPluginInstance *>(host->host_data);
            jassert (h != nullptr);
            jassert (h->plugin != nullptr);

            return h;
        }

        template <typename T>
        void initPluginExtension(const T *&ext, const char *id)
        {
            // plugin extensions need to be initialized on the main thread
            jassert (MessageManager::existsAndIsCurrentThread());

            if (!ext)
                ext = static_cast<const T *>(plugin->get_extension(plugin, id));
        }

        static void clapRequestCallback(const clap_host *host) {
            auto h = fromHost(host);
            h->scheduleMainThreadCallback.set (true);
        }

        static void clapRequestProcess(const clap_host *host) {
            auto h = fromHost(host);
//            h->_scheduleProcess = true; // @TODO
        }

        static void clapRequestRestart(const clap_host *host) {
            auto h = fromHost(host);
//            h->_scheduleRestart = true; // @TODO
        }

        static const void *clapExtension(const clap_host *host, const char *extension) {
            ignoreUnused(host);
            jassert (MessageManager::existsAndIsCurrentThread());

//            if (!strcmp(extension, CLAP_EXT_GUI))
//                return &h->_hostGui;
            if (!strcmp(extension, CLAP_EXT_LOG))
                return &CLAPPluginInstance::hostLog;
//            if (!strcmp(extension, CLAP_EXT_THREAD_CHECK))
//                return &h->_hostThreadCheck;
//            if (!strcmp(extension, CLAP_EXT_TIMER_SUPPORT))
//                return &h->_hostTimerSupport;
//            if (!strcmp(extension, CLAP_EXT_POSIX_FD_SUPPORT))
//                return &h->_hostPosixFdSupport;
            if (!strcmp(extension, CLAP_EXT_PARAMS))
                return &CLAPPluginInstance::hostParams;
            if (!strcmp(extension, CLAP_EXT_STATE))
                return &CLAPPluginInstance::hostState;
            return nullptr;
        }

        /* clap host callbacks */
        static void clapLog(const clap_host *host, clap_log_severity severity, const char *msg)
        {
            ignoreUnused (host);

            switch (severity) {
                case CLAP_LOG_DEBUG:
                case CLAP_LOG_INFO:
                case CLAP_LOG_WARNING:
                case CLAP_LOG_ERROR:
                case CLAP_LOG_FATAL:
                case CLAP_LOG_HOST_MISBEHAVING:
                default:
                    Logger::writeToLog (msg);
                    break;
            }
        }

        std::map<clap_id, CLAPParameter*> paramMap {};
        clap_param_rescan_flags paramRescanFlags { CLAP_INVALID_ID };
        static const constexpr clap_host_log hostLog = {
                CLAPPluginInstance::clapLog,
        };

        void refreshParameterList() override
        {
            if (pluginParams == nullptr)
                return;

            if (paramRescanFlags == CLAP_INVALID_ID)
                paramRescanFlags = CLAP_PARAM_RESCAN_ALL;

            // @TODOD 1. it is forbidden to use CLAP_PARAM_RESCAN_ALL if the plugin is active
//            if (h->isPluginActive() && (flags & CLAP_PARAM_RESCAN_ALL)) {
//                throw std::logic_error(
//                        "clap_host_params.recan(CLAP_PARAM_RESCAN_ALL) was called while the plugin is active!");
//                return;
//            }

            const auto count = pluginParams->count (plugin);
            std::unordered_set<clap_id> paramIds (count * 2);
            AudioProcessorParameterGroup newParameterTree;

            for (uint32_t i = 0; i < count; ++i)
            {
                clap_param_info info {};

                if (! pluginParams->get_info (plugin, i, &info))
                {
                    // Unable to get info for this parameter!
                    jassertfalse;
                    continue;
                }

                if (info.id == CLAP_INVALID_ID)
                {
                    // parameter has an invalid ID
                    jassertfalse;
                    continue;
                }

                if (paramIds.count (info.id) > 0)
                {
                    // parameter is declared twice??
                    jassertfalse;
                    return;
                }

                const auto existingParamIter = paramMap.find (info.id);
                paramIds.insert (info.id);

                if (existingParamIter == paramMap.end()) // adding a new parameter
                {
                    // the plugin should only be adding new parameters when rescanning all of them
                    jassert (paramRescanFlags & CLAP_PARAM_RESCAN_ALL);

                    auto newParam = std::make_unique<CLAPParameter> (info, pluginParams, plugin, hostToPluginParamQueue);
                    paramMap.insert_or_assign (info.id, newParam.get());
                    newParameterTree.addChild (std::move (newParam));
                }
                else // updating an existing parameter
                {
                    //  @TODO: there's probably more to do here...
                    existingParamIter->second->paramInfo = info;
                }

            }

            // remove parameters which are now gone...
            for (auto iter = paramMap.begin(); iter != paramMap.end();)
            {
                if (paramIds.find (iter->first) != paramIds.end())
                {
                    if (paramRescanFlags & CLAP_PARAM_RESCAN_ALL)
                    {
                        auto newParam = std::make_unique<CLAPParameter> (iter->second->paramInfo, pluginParams, plugin, hostToPluginParamQueue);
                        paramMap.insert_or_assign (iter->first, newParam.get());
                        newParameterTree.addChild (std::move (newParam));
                    }
                    ++iter;
                }
                else
                {
                    // a parameter was removed, but the flag CLAP_PARAM_RESCAN_ALL was not specified
                    jassert (paramRescanFlags & CLAP_PARAM_RESCAN_ALL);
                    iter = paramMap.erase (iter);
                }
            }

            if (paramRescanFlags & CLAP_PARAM_RESCAN_ALL)
                setHostedParameterTree (std::move (newParameterTree));

            paramRescanFlags = CLAP_INVALID_ID;
        }

        static void clapParamsRescan(const clap_host *host, clap_param_rescan_flags flags)
        {
            jassert (MessageManager::existsAndIsCurrentThread());

            // @TODO: do something smarter based on the flags
            ignoreUnused (flags);
            auto h = fromHost(host);
            h->paramRescanFlags = flags;
            h->refreshParameterList();
        }

        static void clapParamsClear (const clap_host *host, clap_id param_id, clap_param_clear_flags flags)
        {
            // @TODO
        }

        static void clapParamsRequestFlush(const clap_host *host)
        {
            // @TODO
        }

        static const constexpr clap_host_params hostParams = {
                CLAPPluginInstance::clapParamsRescan,
                CLAPPluginInstance::clapParamsClear,
                CLAPPluginInstance::clapParamsRequestFlush,
        };

        static void clapMarkDirty (const clap_host* host)
        {
            jassert (MessageManager::existsAndIsCurrentThread());
            auto h = fromHost (host);
            h->updateHostDisplay (AudioPluginInstance::ChangeDetails{}.withNonParameterStateChanged (true));
        }

        static const constexpr clap_host_state hostState {
            CLAPPluginInstance::clapMarkDirty,
        };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CLAPPluginInstance)
    };

    //==============================================================================
    CLAPPluginFormat::CLAPPluginFormat()  = default;
    CLAPPluginFormat::~CLAPPluginFormat() = default;

    void CLAPPluginFormat::findAllTypesForFile (OwnedArray<PluginDescription>& results, const String& fileOrIdentifier)
    {
        if (fileMightContainThisPluginType (fileOrIdentifier))
        {
            /**
                Since there is no apparent indication if a VST3 plugin is a shell or not,
                we're stuck iterating through a VST3's factory, creating a description
                for every housed plugin.
            */

            const auto* pluginFactory (juce_clap_helpers::DLLHandleCache::getInstance()->findOrCreateHandle (fileOrIdentifier).getPluginFactory());

            if (pluginFactory != nullptr)
            {
                juce_clap_helpers::DescriptionLister lister (pluginFactory);
                lister.findDescriptionsAndPerform (File (fileOrIdentifier));
                results.addCopiesOf (lister.list);
            }
            else
            {
                jassertfalse;
            }
        }
    }

    static std::unique_ptr<AudioPluginInstance> createCLAPInstance (CLAPPluginFormat& format,
                                                                    const PluginDescription& description)
    {
        if (! format.fileMightContainThisPluginType (description.fileOrIdentifier))
            return nullptr;

        const File file { description.fileOrIdentifier };

        struct ScopedWorkingDirectory
        {
            ~ScopedWorkingDirectory() { previousWorkingDirectory.setAsCurrentWorkingDirectory(); }
            File previousWorkingDirectory = File::getCurrentWorkingDirectory();
        };

        const ScopedWorkingDirectory scope;
        file.getParentDirectory().setAsCurrentWorkingDirectory();

        const auto* pluginFactory (juce_clap_helpers::DLLHandleCache::getInstance()->findOrCreateHandle (file.getFullPathName()).getPluginFactory());
        for (uint32_t i = 0; i < pluginFactory->get_plugin_count (pluginFactory); ++i)
        {
            if (const auto* clapDesc = pluginFactory->get_plugin_descriptor (pluginFactory, i))
            {
                if (juce_clap_helpers::getHashForRange (std::string (clapDesc->id)) == description.uniqueId)
                {
                    auto pluginInstance = std::make_unique<CLAPPluginInstance>(pluginFactory, clapDesc->id, file);

                    if (pluginInstance->initialise())
                        return pluginInstance;
                }
            }
        }

        return {};
    }

    void CLAPPluginFormat::createPluginInstance (const PluginDescription& description,
                                                 double, int, PluginCreationCallback callback)
    {
        auto result = createCLAPInstance (*this, description);

        const auto errorMsg = result == nullptr ? TRANS ("Unable to load XXX plug-in file").replace ("XXX", "CLAP")
                                                : String();

        callback (std::move (result), errorMsg);
    }

    bool CLAPPluginFormat::requiresUnblockedMessageThreadDuringCreation (const PluginDescription&) const
    {
        return false;
    }

    bool CLAPPluginFormat::fileMightContainThisPluginType (const String& fileOrIdentifier)
    {
        auto f = File::createFileWithoutCheckingPath (fileOrIdentifier);

        return f.hasFileExtension (".clap")
#if JUCE_MAC || JUCE_LINUX || JUCE_BSD
               && f.exists();
#else
               && f.existsAsFile();
#endif
    }

    String CLAPPluginFormat::getNameOfPluginFromIdentifier (const String& fileOrIdentifier)
    {
        return fileOrIdentifier; //Impossible to tell because every CLAP is a type of shell...
    }

    bool CLAPPluginFormat::pluginNeedsRescanning (const PluginDescription& description)
    {
        return File (description.fileOrIdentifier).getLastModificationTime() != description.lastFileModTime;
    }

    bool CLAPPluginFormat::doesPluginStillExist (const PluginDescription& description)
    {
        return File (description.fileOrIdentifier).exists();
    }

    StringArray CLAPPluginFormat::searchPathsForPlugins (const FileSearchPath& directoriesToSearch, const bool recursive, bool)
    {
        StringArray results;

        for (int i = 0; i < directoriesToSearch.getNumPaths(); ++i)
            recursiveFileSearch (results, directoriesToSearch[i], recursive);

        return results;
    }

    void CLAPPluginFormat::recursiveFileSearch (StringArray& results, const File& directory, const bool recursive)
    {
        for (const auto& iter : RangedDirectoryIterator (directory, false, "*", File::findFilesAndDirectories))
        {
            auto f = iter.getFile();
            bool isPlugin = false;

            if (fileMightContainThisPluginType (f.getFullPathName()))
            {
                isPlugin = true;
                results.add (f.getFullPathName());
            }

            if (recursive && (! isPlugin) && f.isDirectory())
                recursiveFileSearch (results, f, true);
        }
    }

    FileSearchPath CLAPPluginFormat::getDefaultLocationsToSearch()
    {
        // @TODO: check CLAP_PATH, as documented here: https://github.com/free-audio/clap/blob/main/include/clap/entry.h

#if JUCE_WINDOWS
        const auto localAppData = File::getSpecialLocation (File::windowsLocalAppData)        .getFullPathName();
        const auto programFiles = File::getSpecialLocation (File::globalApplicationsDirectory).getFullPathName();
        return FileSearchPath { localAppData + "\\Programs\\Common\\CLAP;" + programFiles + "\\Common Files\\CLAP" };
#elif JUCE_MAC
        return FileSearchPath { "~/Library/Audio/Plug-Ins/CLAP;/Library/Audio/Plug-Ins/CLAP" };
#else
        return FileSearchPath { "~/.clap/;/usr/lib/clap/" };
#endif
    }
}
