//
//  AudioClient.cpp
//  interface/src
//
//  Created by Stephen Birarda on 1/22/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <cstring>
#include <math.h>
#include <sys/stat.h>

#include <glm/glm.hpp>
#include <glm/gtx/norm.hpp>
#include <glm/gtx/vector_angle.hpp>

#ifdef __APPLE__
#include <CoreAudio/AudioHardware.h>
#endif

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <Mmsystem.h>
#include <mmdeviceapi.h>
#include <devicetopology.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <VersionHelpers.h>
#endif

#include <QtCore/QBuffer>
#include <QtMultimedia/QAudioInput>
#include <QtMultimedia/QAudioOutput>

#include <NodeList.h>
#include <plugins/CodecPlugin.h>
#include <plugins/PluginManager.h>
#include <udt/PacketHeaders.h>
#include <SettingHandle.h>
#include <SharedUtil.h>
#include <Transform.h>

#include "AudioClientLogging.h"
#include "AudioLogging.h"

#include "AudioClient.h"

const int AudioClient::MIN_BUFFER_FRAMES = 1;
const int AudioClient::MAX_BUFFER_FRAMES = 20;

static const int RECEIVED_AUDIO_STREAM_CAPACITY_FRAMES = 100;

static const auto DEFAULT_POSITION_GETTER = []{ return Vectors::ZERO; };
static const auto DEFAULT_ORIENTATION_GETTER = [] { return Quaternions::IDENTITY; };

static const int DEFAULT_BUFFER_FRAMES = 1;

// OUTPUT_CHANNEL_COUNT is audio pipeline output format, which is always 2 channel.
// _outputFormat.channelCount() is device output format, which may be 1 or multichannel.
static const int OUTPUT_CHANNEL_COUNT = 2;

static const bool DEFAULT_STARVE_DETECTION_ENABLED = true;
static const int STARVE_DETECTION_THRESHOLD = 3;
static const int STARVE_DETECTION_PERIOD = 10 * 1000; // 10 Seconds

Setting::Handle<bool> dynamicJitterBufferEnabled("dynamicJitterBuffersEnabled",
    InboundAudioStream::DEFAULT_DYNAMIC_JITTER_BUFFER_ENABLED);
Setting::Handle<int> staticJitterBufferFrames("staticJitterBufferFrames",
    InboundAudioStream::DEFAULT_STATIC_JITTER_FRAMES);

// protect the Qt internal device list
using Mutex = std::mutex;
using Lock = std::unique_lock<Mutex>;
static Mutex _deviceMutex;

// background thread that continuously polls for device changes
class CheckDevicesThread : public QThread {
public:
    const unsigned long DEVICE_CHECK_INTERVAL_MSECS = 2 * 1000;

    CheckDevicesThread(AudioClient* audioClient)
        : _audioClient(audioClient) {
    }

    void beforeAboutToQuit() {
        Lock lock(_checkDevicesMutex);
        _quit = true;
    }

    void run() override {
        while (true) {
            {
                Lock lock(_checkDevicesMutex);
                if (_quit) {
                    break;
                }
                _audioClient->checkDevices();
            }
            QThread::msleep(DEVICE_CHECK_INTERVAL_MSECS);
        }
    }

private:
    AudioClient* _audioClient { nullptr };
    Mutex _checkDevicesMutex;
    bool _quit { false };
};

void AudioInjectorsThread::prepare() {
    _audio->prepareLocalAudioInjectors();
}

static void channelUpmix(int16_t* source, int16_t* dest, int numSamples, int numExtraChannels) {
    for (int i = 0; i < numSamples/2; i++) {

        // read 2 samples
        int16_t left = *source++;
        int16_t right = *source++;

        // write 2 + N samples
        *dest++ = left;
        *dest++ = right;
        for (int n = 0; n < numExtraChannels; n++) {
            *dest++ = 0;
        }
    }
}

static void channelDownmix(int16_t* source, int16_t* dest, int numSamples) {
    for (int i = 0; i < numSamples/2; i++) {

        // read 2 samples
        int16_t left = *source++;
        int16_t right = *source++;

        // write 1 sample
        *dest++ = (int16_t)((left + right) / 2);
    }
}

static inline float convertToFloat(int16_t sample) {
    return (float)sample * (1 / 32768.0f);
}

AudioClient::AudioClient() :
    AbstractAudioInterface(),
    _gate(this),
    _audioInput(NULL),
    _desiredInputFormat(),
    _inputFormat(),
    _numInputCallbackBytes(0),
    _audioOutput(NULL),
    _desiredOutputFormat(),
    _outputFormat(),
    _outputFrameSize(0),
    _numOutputCallbackBytes(0),
    _loopbackAudioOutput(NULL),
    _loopbackOutputDevice(NULL),
    _inputRingBuffer(0),
    _localInjectorsStream(0, 1),
    _receivedAudioStream(RECEIVED_AUDIO_STREAM_CAPACITY_FRAMES),
    _isStereoInput(false),
    _outputStarveDetectionStartTimeMsec(0),
    _outputStarveDetectionCount(0),
    _outputBufferSizeFrames("audioOutputBufferFrames", DEFAULT_BUFFER_FRAMES),
    _sessionOutputBufferSizeFrames(_outputBufferSizeFrames.get()),
    _outputStarveDetectionEnabled("audioOutputStarveDetectionEnabled", DEFAULT_STARVE_DETECTION_ENABLED),
    _lastInputLoudness(0.0f),
    _timeSinceLastClip(-1.0f),
    _muted(false),
    _shouldEchoLocally(false),
    _shouldEchoToServer(false),
    _isNoiseGateEnabled(true),
    _reverb(false),
    _reverbOptions(&_scriptReverbOptions),
    _inputToNetworkResampler(NULL),
    _networkToOutputResampler(NULL),
    _localToOutputResampler(NULL),
    _localAudioThread(this),
    _audioLimiter(AudioConstants::SAMPLE_RATE, OUTPUT_CHANNEL_COUNT),
    _outgoingAvatarAudioSequenceNumber(0),
    _audioOutputIODevice(_localInjectorsStream, _receivedAudioStream, this),
    _stats(&_receivedAudioStream),
    _positionGetter(DEFAULT_POSITION_GETTER),
    _orientationGetter(DEFAULT_ORIENTATION_GETTER) {
    // avoid putting a lock in the device callback
    assert(_localSamplesAvailable.is_lock_free());

    // deprecate legacy settings
    {
        Setting::Handle<int>::Deprecated("maxFramesOverDesired", InboundAudioStream::MAX_FRAMES_OVER_DESIRED);
        Setting::Handle<int>::Deprecated("windowStarveThreshold", InboundAudioStream::WINDOW_STARVE_THRESHOLD);
        Setting::Handle<int>::Deprecated("windowSecondsForDesiredCalcOnTooManyStarves", InboundAudioStream::WINDOW_SECONDS_FOR_DESIRED_CALC_ON_TOO_MANY_STARVES);
        Setting::Handle<int>::Deprecated("windowSecondsForDesiredReduction", InboundAudioStream::WINDOW_SECONDS_FOR_DESIRED_REDUCTION);
        Setting::Handle<bool>::Deprecated("useStDevForJitterCalc", InboundAudioStream::USE_STDEV_FOR_JITTER);
        Setting::Handle<bool>::Deprecated("repetitionWithFade", InboundAudioStream::REPETITION_WITH_FADE);
    }

    connect(&_receivedAudioStream, &MixedProcessedAudioStream::processSamples,
            this, &AudioClient::processReceivedSamples, Qt::DirectConnection);
    connect(this, &AudioClient::changeDevice, this, [=](const QAudioDeviceInfo& outputDeviceInfo) { switchOutputToAudioDevice(outputDeviceInfo); });

    connect(&_receivedAudioStream, &InboundAudioStream::mismatchedAudioCodec, this, &AudioClient::handleMismatchAudioFormat);

    _inputDevices = getDeviceNames(QAudio::AudioInput);
    _outputDevices = getDeviceNames(QAudio::AudioOutput);

    // start a thread to detect any device changes
    _checkDevicesThread = new CheckDevicesThread(this);
    _checkDevicesThread->setObjectName("CheckDevices Thread");
    _checkDevicesThread->setPriority(QThread::LowPriority);
    _checkDevicesThread->start();

    // start a thread to process local injectors
    _localAudioThread.setObjectName("LocalAudio Thread");
    _localAudioThread.start();

    configureReverb();

    auto& packetReceiver = DependencyManager::get<NodeList>()->getPacketReceiver();
    packetReceiver.registerListener(PacketType::AudioStreamStats, &_stats, "processStreamStatsPacket");
    packetReceiver.registerListener(PacketType::AudioEnvironment, this, "handleAudioEnvironmentDataPacket");
    packetReceiver.registerListener(PacketType::SilentAudioFrame, this, "handleAudioDataPacket");
    packetReceiver.registerListener(PacketType::MixedAudio, this, "handleAudioDataPacket");
    packetReceiver.registerListener(PacketType::NoisyMute, this, "handleNoisyMutePacket");
    packetReceiver.registerListener(PacketType::MuteEnvironment, this, "handleMuteEnvironmentPacket");
    packetReceiver.registerListener(PacketType::SelectedAudioFormat, this, "handleSelectedAudioFormat");
}

AudioClient::~AudioClient() {
    delete _checkDevicesThread;
    stop();
    if (_codec && _encoder) {
        _codec->releaseEncoder(_encoder);
        _encoder = nullptr;
    }
}

void AudioClient::beforeAboutToQuit() {
    static_cast<CheckDevicesThread*>(_checkDevicesThread)->beforeAboutToQuit();
}


void AudioClient::handleMismatchAudioFormat(SharedNodePointer node, const QString& currentCodec, const QString& recievedCodec) {
    qCDebug(audioclient) << __FUNCTION__ << "sendingNode:" << *node << "currentCodec:" << currentCodec << "recievedCodec:" << recievedCodec;
    selectAudioFormat(recievedCodec);
}


void AudioClient::reset() {
    _receivedAudioStream.reset();
    _stats.reset();
    _sourceReverb.reset();
    _listenerReverb.reset();
    _localReverb.reset();
}

void AudioClient::audioMixerKilled() {
    _hasReceivedFirstPacket = false;
    _outgoingAvatarAudioSequenceNumber = 0;
    _stats.reset();
    emit disconnected();
}

// thread-safe
QList<QAudioDeviceInfo> getAvailableDevices(QAudio::Mode mode) {
    // NOTE: availableDevices() clobbers the Qt internal device list
    Lock lock(_deviceMutex);
    return QAudioDeviceInfo::availableDevices(mode);
}

QAudioDeviceInfo getNamedAudioDeviceForMode(QAudio::Mode mode, const QString& deviceName) {
    QAudioDeviceInfo result;
    foreach(QAudioDeviceInfo audioDevice, getAvailableDevices(mode)) {
        if (audioDevice.deviceName().trimmed() == deviceName.trimmed()) {
            result = audioDevice;
            break;
        }
    }

    return result;
}

#ifdef Q_OS_WIN
QString friendlyNameForAudioDevice(IMMDevice* pEndpoint) {
    QString deviceName;
    IPropertyStore* pPropertyStore;
    pEndpoint->OpenPropertyStore(STGM_READ, &pPropertyStore);
    pEndpoint->Release();
    pEndpoint = nullptr;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    HRESULT hr = pPropertyStore->GetValue(PKEY_Device_FriendlyName, &pv);
    pPropertyStore->Release();
    pPropertyStore = nullptr;
    deviceName = QString::fromWCharArray((wchar_t*)pv.pwszVal);
    if (!IsWindows8OrGreater()) {
        // Windows 7 provides only the 31 first characters of the device name.
        const DWORD QT_WIN7_MAX_AUDIO_DEVICENAME_LEN = 31;
        deviceName = deviceName.left(QT_WIN7_MAX_AUDIO_DEVICENAME_LEN);
    }
    PropVariantClear(&pv);
    return deviceName;
}

QString AudioClient::friendlyNameForAudioDevice(wchar_t* guid) {
    QString deviceName;
    HRESULT hr = S_OK;
    CoInitialize(nullptr);
    IMMDeviceEnumerator* pMMDeviceEnumerator = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pMMDeviceEnumerator);
    IMMDevice* pEndpoint;
    hr = pMMDeviceEnumerator->GetDevice(guid, &pEndpoint);
    if (hr == E_NOTFOUND) {
        printf("Audio Error: device not found\n");
        deviceName = QString("NONE");
    } else {
        deviceName = ::friendlyNameForAudioDevice(pEndpoint);
    }
    pMMDeviceEnumerator->Release();
    pMMDeviceEnumerator = nullptr;
    CoUninitialize();
    return deviceName;
}

#endif

QAudioDeviceInfo defaultAudioDeviceForMode(QAudio::Mode mode) {
#ifdef __APPLE__
    if (getAvailableDevices(mode).size() > 1) {
        AudioDeviceID defaultDeviceID = 0;
        uint32_t propertySize = sizeof(AudioDeviceID);
        AudioObjectPropertyAddress propertyAddress = {
            kAudioHardwarePropertyDefaultInputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMaster
        };

        if (mode == QAudio::AudioOutput) {
            propertyAddress.mSelector = kAudioHardwarePropertyDefaultOutputDevice;
        }


        OSStatus getPropertyError = AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                                               &propertyAddress,
                                                               0,
                                                               NULL,
                                                               &propertySize,
                                                               &defaultDeviceID);

        if (!getPropertyError && propertySize) {
            CFStringRef deviceName = NULL;
            propertySize = sizeof(deviceName);
            propertyAddress.mSelector = kAudioDevicePropertyDeviceNameCFString;
            getPropertyError = AudioObjectGetPropertyData(defaultDeviceID, &propertyAddress, 0,
                                                          NULL, &propertySize, &deviceName);

            if (!getPropertyError && propertySize) {
                // find a device in the list that matches the name we have and return it
                foreach(QAudioDeviceInfo audioDevice, getAvailableDevices(mode)) {
                    if (audioDevice.deviceName() == CFStringGetCStringPtr(deviceName, kCFStringEncodingMacRoman)) {
                        return audioDevice;
                    }
                }
            }
        }
    }
#endif
#ifdef WIN32
    QString deviceName;
    //Check for Windows Vista or higher, IMMDeviceEnumerator doesn't work below that.
    if (!IsWindowsVistaOrGreater()) { // lower then vista
        if (mode == QAudio::AudioInput) {
            WAVEINCAPS wic;
            // first use WAVE_MAPPER to get the default devices manufacturer ID
            waveInGetDevCaps(WAVE_MAPPER, &wic, sizeof(wic));
            //Use the received manufacturer id to get the device's real name
            waveInGetDevCaps(wic.wMid, &wic, sizeof(wic));
            qCDebug(audioclient) << "input device:" << wic.szPname;
            deviceName = wic.szPname;
        } else {
            WAVEOUTCAPS woc;
            // first use WAVE_MAPPER to get the default devices manufacturer ID
            waveOutGetDevCaps(WAVE_MAPPER, &woc, sizeof(woc));
            //Use the received manufacturer id to get the device's real name
            waveOutGetDevCaps(woc.wMid, &woc, sizeof(woc));
            qCDebug(audioclient) << "output device:" << woc.szPname;
            deviceName = woc.szPname;
        }
    } else {
        HRESULT hr = S_OK;
        CoInitialize(NULL);
        IMMDeviceEnumerator* pMMDeviceEnumerator = NULL;
        CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pMMDeviceEnumerator);
        IMMDevice* pEndpoint;
        hr = pMMDeviceEnumerator->GetDefaultAudioEndpoint(mode == QAudio::AudioOutput ? eRender : eCapture, eMultimedia, &pEndpoint);
        if (hr == E_NOTFOUND) {
            printf("Audio Error: device not found\n");
            deviceName = QString("NONE");
        } else {
            deviceName = friendlyNameForAudioDevice(pEndpoint);
        }
        pMMDeviceEnumerator->Release();
        pMMDeviceEnumerator = NULL;
        CoUninitialize();
    }

    qCDebug(audioclient) << "[" << deviceName << "] [" << getNamedAudioDeviceForMode(mode, deviceName).deviceName() << "]";

    return getNamedAudioDeviceForMode(mode, deviceName);
#endif


    // fallback for failed lookup is the default device
    return (mode == QAudio::AudioInput) ? QAudioDeviceInfo::defaultInputDevice() : QAudioDeviceInfo::defaultOutputDevice();
}

// attempt to use the native sample rate and channel count
bool nativeFormatForAudioDevice(const QAudioDeviceInfo& audioDevice,
                                QAudioFormat& audioFormat) {

    audioFormat = audioDevice.preferredFormat();

    audioFormat.setCodec("audio/pcm");
    audioFormat.setSampleSize(16);
    audioFormat.setSampleType(QAudioFormat::SignedInt);
    audioFormat.setByteOrder(QAudioFormat::LittleEndian);

    if (!audioDevice.isFormatSupported(audioFormat)) {
        qCWarning(audioclient) << "The native format is" << audioFormat << "but isFormatSupported() failed.";
        return false;
    }
    // converting to/from this rate must produce an integral number of samples
    if (audioFormat.sampleRate() * AudioConstants::NETWORK_FRAME_SAMPLES_PER_CHANNEL % AudioConstants::SAMPLE_RATE != 0) {
        qCWarning(audioclient) << "The native sample rate [" << audioFormat.sampleRate() << "] is not supported.";
        return false;
    }
    return true;
}

bool adjustedFormatForAudioDevice(const QAudioDeviceInfo& audioDevice,
                                  const QAudioFormat& desiredAudioFormat,
                                  QAudioFormat& adjustedAudioFormat) {

    qCDebug(audioclient) << "The desired format for audio I/O is" << desiredAudioFormat;

#if defined(Q_OS_ANDROID) || defined(Q_OS_OSX)
    // As of Qt5.6, Android returns the native OpenSLES sample rate when possible, else 48000
    // Mac OSX returns the preferred CoreAudio format
    if (nativeFormatForAudioDevice(audioDevice, adjustedAudioFormat)) {
        return true;
    }
#endif

#if defined(Q_OS_WIN)
    if (IsWindows8OrGreater()) {
        // On Windows using WASAPI shared-mode, returns the internal mix format
        if (nativeFormatForAudioDevice(audioDevice, adjustedAudioFormat)) {
            return true;
        }
    }
#endif

    adjustedAudioFormat = desiredAudioFormat;

    //
    // Attempt the device sample rate and channel count in decreasing order of preference.
    //
    const int sampleRates[] = { 48000, 44100, 32000, 24000, 16000, 96000, 192000, 88200, 176400 };
    const int inputChannels[] = { 1, 2, 4, 6, 8 };  // prefer mono
    const int outputChannels[] = { 2, 4, 6, 8, 1 }; // prefer stereo, downmix as last resort

    for (int channelCount : (desiredAudioFormat.channelCount() == 1 ? inputChannels : outputChannels)) {
        for (int sampleRate : sampleRates) {

            adjustedAudioFormat.setChannelCount(channelCount);
            adjustedAudioFormat.setSampleRate(sampleRate);

            if (audioDevice.isFormatSupported(adjustedAudioFormat)) {
                return true;
            }
        }
    }

    return false;   // a supported format could not be found
}

bool sampleChannelConversion(const int16_t* sourceSamples, int16_t* destinationSamples, unsigned int numSourceSamples,
                             const int sourceChannelCount, const int destinationChannelCount) {
    if (sourceChannelCount == 2 && destinationChannelCount == 1) {
        // loop through the stereo input audio samples and average every two samples
        for (uint i = 0; i < numSourceSamples; i += 2) {
            destinationSamples[i / 2] = (sourceSamples[i] / 2) + (sourceSamples[i + 1] / 2);
        }

        return true;
    } else if (sourceChannelCount == 1 && destinationChannelCount == 2) {

        // loop through the mono input audio and repeat each sample twice
        for (uint i = 0; i < numSourceSamples; ++i) {
            destinationSamples[i * 2] = destinationSamples[(i * 2) + 1] = sourceSamples[i];
        }

        return true;
    }

    return false;
}

void possibleResampling(AudioSRC* resampler,
                        const int16_t* sourceSamples, int16_t* destinationSamples,
                        unsigned int numSourceSamples, unsigned int numDestinationSamples,
                        const int sourceChannelCount, const int destinationChannelCount) {

    if (numSourceSamples > 0) {
        if (!resampler) {
            if (!sampleChannelConversion(sourceSamples, destinationSamples, numSourceSamples,
                                         sourceChannelCount, destinationChannelCount)) {
                // no conversion, we can copy the samples directly across
                memcpy(destinationSamples, sourceSamples, numSourceSamples * AudioConstants::SAMPLE_SIZE);
            }
        } else {

            if (sourceChannelCount != destinationChannelCount) {

                int numChannelCoversionSamples = (numSourceSamples * destinationChannelCount) / sourceChannelCount;
                int16_t* channelConversionSamples = new int16_t[numChannelCoversionSamples];

                sampleChannelConversion(sourceSamples, channelConversionSamples, numSourceSamples,
                                        sourceChannelCount, destinationChannelCount);

                resampler->render(channelConversionSamples, destinationSamples, numChannelCoversionSamples);

                delete[] channelConversionSamples;
            } else {

                unsigned int numAdjustedSourceSamples = numSourceSamples;
                unsigned int numAdjustedDestinationSamples = numDestinationSamples;

                if (sourceChannelCount == 2 && destinationChannelCount == 2) {
                    numAdjustedSourceSamples /= 2;
                    numAdjustedDestinationSamples /= 2;
                }

                resampler->render(sourceSamples, destinationSamples, numAdjustedSourceSamples);
            }
        }
    }
}

void AudioClient::start() {

    // set up the desired audio format
    _desiredInputFormat.setSampleRate(AudioConstants::SAMPLE_RATE);
    _desiredInputFormat.setSampleSize(16);
    _desiredInputFormat.setCodec("audio/pcm");
    _desiredInputFormat.setSampleType(QAudioFormat::SignedInt);
    _desiredInputFormat.setByteOrder(QAudioFormat::LittleEndian);
    _desiredInputFormat.setChannelCount(1);

    _desiredOutputFormat = _desiredInputFormat;
    _desiredOutputFormat.setChannelCount(OUTPUT_CHANNEL_COUNT);

    QAudioDeviceInfo inputDeviceInfo = defaultAudioDeviceForMode(QAudio::AudioInput);
    qCDebug(audioclient) << "The default audio input device is" << inputDeviceInfo.deviceName();
    bool inputFormatSupported = switchInputToAudioDevice(inputDeviceInfo);

    QAudioDeviceInfo outputDeviceInfo = defaultAudioDeviceForMode(QAudio::AudioOutput);
    qCDebug(audioclient) << "The default audio output device is" << outputDeviceInfo.deviceName();
    bool outputFormatSupported = switchOutputToAudioDevice(outputDeviceInfo);

    if (!inputFormatSupported) {
        qCDebug(audioclient) << "Unable to set up audio input because of a problem with input format.";
        qCDebug(audioclient) << "The closest format available is" << inputDeviceInfo.nearestFormat(_desiredInputFormat);
    }

    if (!outputFormatSupported) {
        qCDebug(audioclient) << "Unable to set up audio output because of a problem with output format.";
        qCDebug(audioclient) << "The closest format available is" << outputDeviceInfo.nearestFormat(_desiredOutputFormat);
    }
}

void AudioClient::stop() {
    // "switch" to invalid devices in order to shut down the state
    switchInputToAudioDevice(QAudioDeviceInfo());
    switchOutputToAudioDevice(QAudioDeviceInfo());
}

void AudioClient::handleAudioEnvironmentDataPacket(QSharedPointer<ReceivedMessage> message) {

    char bitset;
    message->readPrimitive(&bitset);

    bool hasReverb = oneAtBit(bitset, HAS_REVERB_BIT);
    
    if (hasReverb) {
        float reverbTime, wetLevel;
        message->readPrimitive(&reverbTime);
        message->readPrimitive(&wetLevel);
        _receivedAudioStream.setReverb(reverbTime, wetLevel);
    } else {
        _receivedAudioStream.clearReverb();
   }
}

void AudioClient::handleAudioDataPacket(QSharedPointer<ReceivedMessage> message) {

    if (message->getType() == PacketType::SilentAudioFrame) {
        _silentInbound.increment();
    } else {
        _audioInbound.increment();
    }

    auto nodeList = DependencyManager::get<NodeList>();
    nodeList->flagTimeForConnectionStep(LimitedNodeList::ConnectionStep::ReceiveFirstAudioPacket);

    if (_audioOutput) {

        if (!_hasReceivedFirstPacket) {
            _hasReceivedFirstPacket = true;

            // have the audio scripting interface emit a signal to say we just connected to mixer
            emit receivedFirstPacket();
        }

#if DEV_BUILD || PR_BUILD
        _gate.insert(message);
#else
        // Audio output must exist and be correctly set up if we're going to process received audio
        _receivedAudioStream.parseData(*message);
#endif
    }
}

AudioClient::Gate::Gate(AudioClient* audioClient) :
    _audioClient(audioClient) {}

void AudioClient::Gate::setIsSimulatingJitter(bool enable) {
    std::lock_guard<std::mutex> lock(_mutex);
    flush();
    _isSimulatingJitter = enable;
}

void AudioClient::Gate::setThreshold(int threshold) {
    std::lock_guard<std::mutex> lock(_mutex);
    flush();
    _threshold = std::max(threshold, 1);
}

void AudioClient::Gate::insert(QSharedPointer<ReceivedMessage> message) {
    std::lock_guard<std::mutex> lock(_mutex);

    // Short-circuit for normal behavior
    if (_threshold == 1 && !_isSimulatingJitter) {
        _audioClient->_receivedAudioStream.parseData(*message);
        return;
    }

    // Throttle the current packet until the next flush
    _queue.push(message);
    _index++;

    // When appropriate, flush all held packets to the received audio stream
    if (_isSimulatingJitter) {
        // The JITTER_FLUSH_CHANCE defines the discrete probability density function of jitter (ms),
        // where f(t) = pow(1 - JITTER_FLUSH_CHANCE, (t / 10) * JITTER_FLUSH_CHANCE
        // for t (ms) = 10, 20, ... (because typical packet timegap is 10ms),
        // because there is a JITTER_FLUSH_CHANCE of any packet instigating a flush of all held packets.
        static const float JITTER_FLUSH_CHANCE = 0.6f;
        // It is set at 0.6 to give a low chance of spikes (>30ms, 2.56%) so that they are obvious,
        // but settled within the measured 5s window in audio network stats.
        if (randFloat() < JITTER_FLUSH_CHANCE) {
            flush();
        }
    } else if (!(_index % _threshold)) {
        flush();
    }
}

void AudioClient::Gate::flush() {
    // Send all held packets to the received audio stream to be (eventually) played
    while (!_queue.empty()) {
        _audioClient->_receivedAudioStream.parseData(*_queue.front());
        _queue.pop();
    }
    _index = 0;
}


void AudioClient::handleNoisyMutePacket(QSharedPointer<ReceivedMessage> message) {
    if (!_muted) {
        toggleMute();
        
        // have the audio scripting interface emit a signal to say we were muted by the mixer
        emit mutedByMixer();
    }
}

void AudioClient::handleMuteEnvironmentPacket(QSharedPointer<ReceivedMessage> message) {
    glm::vec3 position;
    float radius;
    
    message->readPrimitive(&position);
    message->readPrimitive(&radius);

    emit muteEnvironmentRequested(position, radius);
}

void AudioClient::negotiateAudioFormat() {
    auto nodeList = DependencyManager::get<NodeList>();
    auto negotiateFormatPacket = NLPacket::create(PacketType::NegotiateAudioFormat);
    auto codecPlugins = PluginManager::getInstance()->getCodecPlugins();
    quint8 numberOfCodecs = (quint8)codecPlugins.size();
    negotiateFormatPacket->writePrimitive(numberOfCodecs);
    for (auto& plugin : codecPlugins) {
        auto codecName = plugin->getName();
        negotiateFormatPacket->writeString(codecName);
    }

    // grab our audio mixer from the NodeList, if it exists
    SharedNodePointer audioMixer = nodeList->soloNodeOfType(NodeType::AudioMixer);

    if (audioMixer) {
        // send off this mute packet
        nodeList->sendPacket(std::move(negotiateFormatPacket), *audioMixer);
    }
}

void AudioClient::handleSelectedAudioFormat(QSharedPointer<ReceivedMessage> message) {
    QString selectedCodecName = message->readString();
    selectAudioFormat(selectedCodecName);
}

void AudioClient::selectAudioFormat(const QString& selectedCodecName) {
    
    _selectedCodecName = selectedCodecName;

    qCDebug(audioclient) << "Selected Codec:" << _selectedCodecName;

    // release any old codec encoder/decoder first...
    if (_codec && _encoder) {
        _codec->releaseEncoder(_encoder);
        _encoder = nullptr;
        _codec = nullptr;
    }
    _receivedAudioStream.cleanupCodec();

    auto codecPlugins = PluginManager::getInstance()->getCodecPlugins();
    for (auto& plugin : codecPlugins) {
        if (_selectedCodecName == plugin->getName()) {
            _codec = plugin;
            _receivedAudioStream.setupCodec(plugin, _selectedCodecName, AudioConstants::STEREO); 
            _encoder = plugin->createEncoder(AudioConstants::SAMPLE_RATE, AudioConstants::MONO);
            qCDebug(audioclient) << "Selected Codec Plugin:" << _codec.get();
            break;
        }
    }

}
   

QString AudioClient::getDefaultDeviceName(QAudio::Mode mode) {
    QAudioDeviceInfo deviceInfo = defaultAudioDeviceForMode(mode);
    return deviceInfo.deviceName();
}

QVector<QString> AudioClient::getDeviceNames(QAudio::Mode mode) {
    QVector<QString> deviceNames;
    foreach(QAudioDeviceInfo audioDevice, getAvailableDevices(mode)) {
        deviceNames << audioDevice.deviceName().trimmed();
    }
    return deviceNames;
}

bool AudioClient::switchInputToAudioDevice(const QString& inputDeviceName) {
    qCDebug(audioclient) << "[" << inputDeviceName << "] [" << getNamedAudioDeviceForMode(QAudio::AudioInput, inputDeviceName).deviceName() << "]";
    return switchInputToAudioDevice(getNamedAudioDeviceForMode(QAudio::AudioInput, inputDeviceName));
}

bool AudioClient::switchOutputToAudioDevice(const QString& outputDeviceName) {
    qCDebug(audioclient) << "[" << outputDeviceName << "] [" << getNamedAudioDeviceForMode(QAudio::AudioOutput, outputDeviceName).deviceName() << "]";
    return switchOutputToAudioDevice(getNamedAudioDeviceForMode(QAudio::AudioOutput, outputDeviceName));
}

void AudioClient::configureReverb() {
    ReverbParameters p;

    p.sampleRate = AudioConstants::SAMPLE_RATE;
    p.bandwidth = _reverbOptions->getBandwidth();
    p.preDelay = _reverbOptions->getPreDelay();
    p.lateDelay = _reverbOptions->getLateDelay();
    p.reverbTime = _reverbOptions->getReverbTime();
    p.earlyDiffusion = _reverbOptions->getEarlyDiffusion();
    p.lateDiffusion = _reverbOptions->getLateDiffusion();
    p.roomSize = _reverbOptions->getRoomSize();
    p.density = _reverbOptions->getDensity();
    p.bassMult = _reverbOptions->getBassMult();
    p.bassFreq = _reverbOptions->getBassFreq();
    p.highGain = _reverbOptions->getHighGain();
    p.highFreq = _reverbOptions->getHighFreq();
    p.modRate = _reverbOptions->getModRate();
    p.modDepth = _reverbOptions->getModDepth();
    p.earlyGain = _reverbOptions->getEarlyGain();
    p.lateGain = _reverbOptions->getLateGain();
    p.earlyMixLeft = _reverbOptions->getEarlyMixLeft();
    p.earlyMixRight = _reverbOptions->getEarlyMixRight();
    p.lateMixLeft = _reverbOptions->getLateMixLeft();
    p.lateMixRight = _reverbOptions->getLateMixRight();
    p.wetDryMix = _reverbOptions->getWetDryMix();

    _listenerReverb.setParameters(&p);
    _localReverb.setParameters(&p);

    // used only for adding self-reverb to loopback audio
    p.sampleRate = _outputFormat.sampleRate();
    p.wetDryMix = 100.0f;
    p.preDelay = 0.0f;
    p.earlyGain = -96.0f;   // disable ER
    p.lateGain += _reverbOptions->getWetDryMix() * (24.0f/100.0f) - 24.0f;  // -0dB to -24dB, based on wetDryMix
    p.lateMixLeft = 0.0f;
    p.lateMixRight = 0.0f;

    _sourceReverb.setParameters(&p);
}

void AudioClient::updateReverbOptions() {
    bool reverbChanged = false;
    if (_receivedAudioStream.hasReverb()) {

        if (_zoneReverbOptions.getReverbTime() != _receivedAudioStream.getRevebTime()) {
            _zoneReverbOptions.setReverbTime(_receivedAudioStream.getRevebTime());
            reverbChanged = true;
        }
        if (_zoneReverbOptions.getWetDryMix() != _receivedAudioStream.getWetLevel()) {
            _zoneReverbOptions.setWetDryMix(_receivedAudioStream.getWetLevel());
            reverbChanged = true;
        }

        if (_reverbOptions != &_zoneReverbOptions) {
            _reverbOptions = &_zoneReverbOptions;
            reverbChanged = true;
        }
    } else if (_reverbOptions != &_scriptReverbOptions) {
        _reverbOptions = &_scriptReverbOptions;
        reverbChanged = true;
    }

    if (reverbChanged) {
        configureReverb();
    }
}

void AudioClient::setReverb(bool reverb) {
    _reverb = reverb;

    if (!_reverb) {
        _sourceReverb.reset();
        _listenerReverb.reset();
        _localReverb.reset();
    }
}

void AudioClient::setReverbOptions(const AudioEffectOptions* options) {
    // Save the new options
    _scriptReverbOptions.setBandwidth(options->getBandwidth());
    _scriptReverbOptions.setPreDelay(options->getPreDelay());
    _scriptReverbOptions.setLateDelay(options->getLateDelay());
    _scriptReverbOptions.setReverbTime(options->getReverbTime());
    _scriptReverbOptions.setEarlyDiffusion(options->getEarlyDiffusion());
    _scriptReverbOptions.setLateDiffusion(options->getLateDiffusion());
    _scriptReverbOptions.setRoomSize(options->getRoomSize());
    _scriptReverbOptions.setDensity(options->getDensity());
    _scriptReverbOptions.setBassMult(options->getBassMult());
    _scriptReverbOptions.setBassFreq(options->getBassFreq());
    _scriptReverbOptions.setHighGain(options->getHighGain());
    _scriptReverbOptions.setHighFreq(options->getHighFreq());
    _scriptReverbOptions.setModRate(options->getModRate());
    _scriptReverbOptions.setModDepth(options->getModDepth());
    _scriptReverbOptions.setEarlyGain(options->getEarlyGain());
    _scriptReverbOptions.setLateGain(options->getLateGain());
    _scriptReverbOptions.setEarlyMixLeft(options->getEarlyMixLeft());
    _scriptReverbOptions.setEarlyMixRight(options->getEarlyMixRight());
    _scriptReverbOptions.setLateMixLeft(options->getLateMixLeft());
    _scriptReverbOptions.setLateMixRight(options->getLateMixRight());
    _scriptReverbOptions.setWetDryMix(options->getWetDryMix());

    if (_reverbOptions == &_scriptReverbOptions) {
        // Apply them to the reverb instances
        configureReverb();
    }
}

void AudioClient::handleLocalEchoAndReverb(QByteArray& inputByteArray) {
    // If there is server echo, reverb will be applied to the recieved audio stream so no need to have it here.
    bool hasReverb = _reverb || _receivedAudioStream.hasReverb();
    if (_muted || !_audioOutput || (!_shouldEchoLocally && !hasReverb)) {
        return;
    }

    // NOTE: we assume the inputFormat and the outputFormat are the same, since on any modern
    // multimedia OS they should be. If there is a device that this is not true for, we can
    // add back support to do resampling.
    if (_inputFormat.sampleRate() != _outputFormat.sampleRate()) {
        return;
    }

    // if this person wants local loopback add that to the locally injected audio
    // if there is reverb apply it to local audio and substract the origin samples

    if (!_loopbackOutputDevice && _loopbackAudioOutput) {
        // we didn't have the loopback output device going so set that up now

        // NOTE: device start() uses the Qt internal device list
        Lock lock(_deviceMutex);
        _loopbackOutputDevice = _loopbackAudioOutput->start();
        lock.unlock();

        if (!_loopbackOutputDevice) {
            return;
        }
    }

    static QByteArray loopBackByteArray;

    int numInputSamples = inputByteArray.size() / AudioConstants::SAMPLE_SIZE;
    int numLoopbackSamples = (numInputSamples * OUTPUT_CHANNEL_COUNT) / _inputFormat.channelCount();

    loopBackByteArray.resize(numLoopbackSamples * AudioConstants::SAMPLE_SIZE);

    int16_t* inputSamples = reinterpret_cast<int16_t*>(inputByteArray.data());
    int16_t* loopbackSamples = reinterpret_cast<int16_t*>(loopBackByteArray.data());

    // upmix mono to stereo
    if (!sampleChannelConversion(inputSamples, loopbackSamples, numInputSamples, _inputFormat.channelCount(), OUTPUT_CHANNEL_COUNT)) {
        // no conversion, just copy the samples
        memcpy(loopbackSamples, inputSamples, numInputSamples * AudioConstants::SAMPLE_SIZE);
    }

    // apply stereo reverb at the source, to the loopback audio
    if (!_shouldEchoLocally && hasReverb) {
        updateReverbOptions();
        _sourceReverb.render(loopbackSamples, loopbackSamples, numLoopbackSamples/2);
    }

    // if required, upmix or downmix to deviceChannelCount
    int deviceChannelCount = _outputFormat.channelCount();
    if (deviceChannelCount == OUTPUT_CHANNEL_COUNT) {

        _loopbackOutputDevice->write(loopBackByteArray);

    } else {

        static QByteArray deviceByteArray;

        int numDeviceSamples = (numLoopbackSamples * deviceChannelCount) / OUTPUT_CHANNEL_COUNT;

        deviceByteArray.resize(numDeviceSamples * AudioConstants::SAMPLE_SIZE);

        int16_t* deviceSamples = reinterpret_cast<int16_t*>(deviceByteArray.data());

        if (deviceChannelCount > OUTPUT_CHANNEL_COUNT) {
            channelUpmix(loopbackSamples, deviceSamples, numLoopbackSamples, deviceChannelCount - OUTPUT_CHANNEL_COUNT);
        } else {
            channelDownmix(loopbackSamples, deviceSamples, numLoopbackSamples);
        }
        _loopbackOutputDevice->write(deviceByteArray);
    }
}

void AudioClient::handleAudioInput(QByteArray& audioBuffer) {
    if (_muted) {
        _lastInputLoudness = 0.0f;
        _timeSinceLastClip = 0.0f;
    } else {
        int16_t* samples = reinterpret_cast<int16_t*>(audioBuffer.data());
        int numSamples = audioBuffer.size() / sizeof(AudioConstants::SAMPLE_SIZE);
        bool didClip = false;

        bool shouldRemoveDCOffset = !_isPlayingBackRecording && !_isStereoInput;
        if (shouldRemoveDCOffset) {
            _noiseGate.removeDCOffset(samples, numSamples);
        }

        bool shouldNoiseGate = (_isPlayingBackRecording || !_isStereoInput) && _isNoiseGateEnabled;
        if (shouldNoiseGate) {
            _noiseGate.gateSamples(samples, numSamples);
            _lastInputLoudness = _noiseGate.getLastLoudness();
            didClip = _noiseGate.clippedInLastBlock();
        } else {
            float loudness = 0.0f;
            for (int i = 0; i < numSamples; ++i) {
                int16_t sample = std::abs(samples[i]);
                loudness += (float)sample;
                didClip = didClip ||
                    (sample > (AudioConstants::MAX_SAMPLE_VALUE * AudioNoiseGate::CLIPPING_THRESHOLD));
            }
            _lastInputLoudness = fabs(loudness / numSamples);
        }

        if (didClip) {
            _timeSinceLastClip = 0.0f;
        } else if (_timeSinceLastClip >= 0.0f) {
            _timeSinceLastClip += (float)numSamples / (float)AudioConstants::SAMPLE_RATE;
        }

        emit inputReceived(audioBuffer);

        if (_noiseGate.openedInLastBlock()) {
            emit noiseGateOpened();
        } else if (_noiseGate.closedInLastBlock()) {
            emit noiseGateClosed();
        }
    }

    // the codec needs a flush frame before sending silent packets, so
    // do not send one if the gate closed in this block (eventually this can be crossfaded).
    auto packetType = _shouldEchoToServer ?
        PacketType::MicrophoneAudioWithEcho : PacketType::MicrophoneAudioNoEcho;
    if (_lastInputLoudness == 0.0f && !_noiseGate.closedInLastBlock()) {
        packetType = PacketType::SilentAudioFrame;
        _silentOutbound.increment();
    } else {
        _audioOutbound.increment();
    }

    Transform audioTransform;
    audioTransform.setTranslation(_positionGetter());
    audioTransform.setRotation(_orientationGetter());

    QByteArray encodedBuffer;
    if (_encoder) {
        _encoder->encode(audioBuffer, encodedBuffer);
    } else {
        encodedBuffer = audioBuffer;
    }

    emitAudioPacket(encodedBuffer.data(), encodedBuffer.size(), _outgoingAvatarAudioSequenceNumber,
            audioTransform, avatarBoundingBoxCorner, avatarBoundingBoxScale,
            packetType, _selectedCodecName);
    _stats.sentPacket();
}

void AudioClient::handleMicAudioInput() {
    if (!_inputDevice || _isPlayingBackRecording) {
        return;
    }

    // input samples required to produce exactly NETWORK_FRAME_SAMPLES of output
    const int inputSamplesRequired = (_inputToNetworkResampler ?
                                      _inputToNetworkResampler->getMinInput(AudioConstants::NETWORK_FRAME_SAMPLES_PER_CHANNEL) :
                                      AudioConstants::NETWORK_FRAME_SAMPLES_PER_CHANNEL) * _inputFormat.channelCount();

    const auto inputAudioSamples = std::unique_ptr<int16_t[]>(new int16_t[inputSamplesRequired]);
    QByteArray inputByteArray = _inputDevice->readAll();

    handleLocalEchoAndReverb(inputByteArray);

    _inputRingBuffer.writeData(inputByteArray.data(), inputByteArray.size());

    float audioInputMsecsRead = inputByteArray.size() / (float)(_inputFormat.bytesForDuration(USECS_PER_MSEC));
    _stats.updateInputMsRead(audioInputMsecsRead);

    const int numNetworkBytes = _isStereoInput
        ? AudioConstants::NETWORK_FRAME_BYTES_STEREO
        : AudioConstants::NETWORK_FRAME_BYTES_PER_CHANNEL;
    const int numNetworkSamples = _isStereoInput
        ? AudioConstants::NETWORK_FRAME_SAMPLES_STEREO
        : AudioConstants::NETWORK_FRAME_SAMPLES_PER_CHANNEL;

    static int16_t networkAudioSamples[AudioConstants::NETWORK_FRAME_SAMPLES_STEREO];

    while (_inputRingBuffer.samplesAvailable() >= inputSamplesRequired) {
        if (_muted) {
            _inputRingBuffer.shiftReadPosition(inputSamplesRequired);
        } else {
            _inputRingBuffer.readSamples(inputAudioSamples.get(), inputSamplesRequired);
            possibleResampling(_inputToNetworkResampler,
                inputAudioSamples.get(), networkAudioSamples,
                inputSamplesRequired, numNetworkSamples,
                _inputFormat.channelCount(), _desiredInputFormat.channelCount());
        }
        int bytesInInputRingBuffer = _inputRingBuffer.samplesAvailable() * AudioConstants::SAMPLE_SIZE;
        float msecsInInputRingBuffer = bytesInInputRingBuffer / (float)(_inputFormat.bytesForDuration(USECS_PER_MSEC));
        _stats.updateInputMsUnplayed(msecsInInputRingBuffer);

        QByteArray audioBuffer(reinterpret_cast<char*>(networkAudioSamples), numNetworkBytes);
        handleAudioInput(audioBuffer);
    }
}

void AudioClient::handleRecordedAudioInput(const QByteArray& audio) {
    QByteArray audioBuffer(audio);
    handleAudioInput(audioBuffer);
}

void AudioClient::prepareLocalAudioInjectors() {
    if (_outputPeriod == 0) {
        return;
    }

    int bufferCapacity = _localInjectorsStream.getSampleCapacity();
    if (_localToOutputResampler) {
        // avoid overwriting the buffer,
        // instead of failing on writes because the buffer is used as a lock-free pipe
        bufferCapacity -=
            _localToOutputResampler->getMaxOutput(AudioConstants::NETWORK_FRAME_SAMPLES_PER_CHANNEL) *
            AudioConstants::STEREO;
        bufferCapacity += 1;
    }

    int samplesNeeded = std::numeric_limits<int>::max();
    while (samplesNeeded > 0) {
        // lock for every write to avoid locking out the device callback
        // this lock is intentional - the buffer is only lock-free in its use in the device callback
        RecursiveLock lock(_localAudioMutex);

        samplesNeeded = bufferCapacity - _localSamplesAvailable.load(std::memory_order_relaxed);
        if (samplesNeeded <= 0) {
            break;
        }

        // get a network frame of local injectors' audio
        if (!mixLocalAudioInjectors(_localMixBuffer)) {
            break;
        }

        // reverb
        if (_reverb) {
            _localReverb.render(_localMixBuffer, _localMixBuffer, AudioConstants::NETWORK_FRAME_SAMPLES_PER_CHANNEL);
        }

        int samples;
        if (_localToOutputResampler) {
            // resample to output sample rate
            int frames = _localToOutputResampler->render(_localMixBuffer, _localOutputMixBuffer,
                AudioConstants::NETWORK_FRAME_SAMPLES_PER_CHANNEL);

            // write to local injectors' ring buffer
            samples = frames * AudioConstants::STEREO;
            _localInjectorsStream.writeSamples(_localOutputMixBuffer, samples);

        } else {
            // write to local injectors' ring buffer
            samples = AudioConstants::NETWORK_FRAME_SAMPLES_STEREO;
            _localInjectorsStream.writeSamples(_localMixBuffer,
                AudioConstants::NETWORK_FRAME_SAMPLES_STEREO);
        }

        _localSamplesAvailable.fetch_add(samples, std::memory_order_release);
        samplesNeeded -= samples;
    }
}

bool AudioClient::mixLocalAudioInjectors(float* mixBuffer) {

    QVector<AudioInjector*> injectorsToRemove;
    
    // lock the injector vector
    Lock lock(_injectorsMutex);

    if (_activeLocalAudioInjectors.size() == 0) {
        return false;
    }

    memset(mixBuffer, 0, AudioConstants::NETWORK_FRAME_SAMPLES_STEREO * sizeof(float));

    for (AudioInjector* injector : _activeLocalAudioInjectors) {
        if (injector->getLocalBuffer()) {

            static const int HRTF_DATASET_INDEX = 1;

            int numChannels = injector->isAmbisonic() ? AudioConstants::AMBISONIC : (injector->isStereo() ? AudioConstants::STEREO : AudioConstants::MONO);
            qint64 bytesToRead = numChannels * AudioConstants::NETWORK_FRAME_BYTES_PER_CHANNEL;

            // get one frame from the injector
            memset(_localScratchBuffer, 0, bytesToRead);
            if (0 < injector->getLocalBuffer()->readData((char*)_localScratchBuffer, bytesToRead)) {
                
                if (injector->isAmbisonic()) {

                    // no distance attenuation
                    float gain = injector->getVolume();

                    //
                    // Calculate the soundfield orientation relative to the listener.
                    // Injector orientation can be used to align a recording to our world coordinates.
                    //
                    glm::quat relativeOrientation = injector->getOrientation() * glm::inverse(_orientationGetter());

                    // convert from Y-up (OpenGL) to Z-up (Ambisonic) coordinate system
                    float qw = relativeOrientation.w;
                    float qx = -relativeOrientation.z;
                    float qy = -relativeOrientation.x;
                    float qz = relativeOrientation.y;

                    // Ambisonic gets spatialized into mixBuffer
                    injector->getLocalFOA().render(_localScratchBuffer, mixBuffer, HRTF_DATASET_INDEX,
                                                   qw, qx, qy, qz, gain, AudioConstants::NETWORK_FRAME_SAMPLES_PER_CHANNEL);

                } else if (injector->isStereo()) {

                    // stereo gets directly mixed into mixBuffer
                    float gain = injector->getVolume();
                    for (int i = 0; i < AudioConstants::NETWORK_FRAME_SAMPLES_STEREO; i++) {
                        mixBuffer[i] += convertToFloat(_localScratchBuffer[i]) * gain;
                    }
                    
                } else {

                    // calculate distance, gain and azimuth for hrtf
                    glm::vec3 relativePosition = injector->getPosition() - _positionGetter();
                    float distance = glm::max(glm::length(relativePosition), EPSILON);
                    float gain = gainForSource(distance, injector->getVolume()); 
                    float azimuth = azimuthForSource(relativePosition);
                
                    // mono gets spatialized into mixBuffer
                    injector->getLocalHRTF().render(_localScratchBuffer, mixBuffer, HRTF_DATASET_INDEX, 
                                                    azimuth, distance, gain, AudioConstants::NETWORK_FRAME_SAMPLES_PER_CHANNEL);
                }
            
            } else {
                
                qCDebug(audioclient) << "injector has no more data, marking finished for removal";
                injector->finishLocalInjection();
                injectorsToRemove.append(injector);
            }

        } else {
            
            qCDebug(audioclient) << "injector has no local buffer, marking as finished for removal";
            injector->finishLocalInjection();
            injectorsToRemove.append(injector);
        }
    }
    
    for (AudioInjector* injector : injectorsToRemove) {
        qCDebug(audioclient) << "removing injector";
        _activeLocalAudioInjectors.removeOne(injector);
    }

    return true;
}

void AudioClient::processReceivedSamples(const QByteArray& decodedBuffer, QByteArray& outputBuffer) {

    const int16_t* decodedSamples = reinterpret_cast<const int16_t*>(decodedBuffer.data());
    assert(decodedBuffer.size() == AudioConstants::NETWORK_FRAME_BYTES_STEREO);

    outputBuffer.resize(_outputFrameSize * AudioConstants::SAMPLE_SIZE);
    int16_t* outputSamples = reinterpret_cast<int16_t*>(outputBuffer.data());

    bool hasReverb = _reverb || _receivedAudioStream.hasReverb();

    // apply stereo reverb
    if (hasReverb) {
        updateReverbOptions();
        int16_t* reverbSamples = _networkToOutputResampler ? _networkScratchBuffer : outputSamples;
        _listenerReverb.render(decodedSamples, reverbSamples, AudioConstants::NETWORK_FRAME_SAMPLES_PER_CHANNEL);
    }

    // resample to output sample rate
    if (_networkToOutputResampler) {
        const int16_t* inputSamples = hasReverb ? _networkScratchBuffer : decodedSamples;
        _networkToOutputResampler->render(inputSamples, outputSamples, AudioConstants::NETWORK_FRAME_SAMPLES_PER_CHANNEL);
    }

    // if no transformations were applied, we still need to copy the buffer
    if (!hasReverb && !_networkToOutputResampler) {
        memcpy(outputSamples, decodedSamples, decodedBuffer.size());
    }
}

void AudioClient::sendMuteEnvironmentPacket() {
    auto nodeList = DependencyManager::get<NodeList>();

    int dataSize = sizeof(glm::vec3) + sizeof(float);

    auto mutePacket = NLPacket::create(PacketType::MuteEnvironment, dataSize);

    const float MUTE_RADIUS = 50;

    glm::vec3 currentSourcePosition = _positionGetter();

    mutePacket->writePrimitive(currentSourcePosition);
    mutePacket->writePrimitive(MUTE_RADIUS);

    // grab our audio mixer from the NodeList, if it exists
    SharedNodePointer audioMixer = nodeList->soloNodeOfType(NodeType::AudioMixer);

    if (audioMixer) {
        // send off this mute packet
        nodeList->sendPacket(std::move(mutePacket), *audioMixer);
    }
}

void AudioClient::toggleMute() {
    _muted = !_muted;
    emit muteToggled();
}

void AudioClient::setIsStereoInput(bool isStereoInput) {
    if (isStereoInput != _isStereoInput) {
        _isStereoInput = isStereoInput;

        if (_isStereoInput) {
            _desiredInputFormat.setChannelCount(2);
        } else {
            _desiredInputFormat.setChannelCount(1);
        }

        // change in channel count for desired input format, restart the input device
        switchInputToAudioDevice(_inputAudioDeviceName);
    }
}

bool AudioClient::outputLocalInjector(AudioInjector* injector) {
    Lock lock(_injectorsMutex);
    if (injector->getLocalBuffer() && _audioInput ) {
        // just add it to the vector of active local injectors, if 
        // not already there.
        // Since this is invoked with invokeMethod, there _should_ be
        // no reason to lock access to the vector of injectors.
        if (!_activeLocalAudioInjectors.contains(injector)) {
            qCDebug(audioclient) << "adding new injector";
            _activeLocalAudioInjectors.append(injector);
        } else {
            qCDebug(audioclient) << "injector exists in active list already";
        }
        
        return true;

    } else {
        // no local buffer or audio
        return false;
    }
}

void AudioClient::outputFormatChanged() {
    _outputFrameSize = (AudioConstants::NETWORK_FRAME_SAMPLES_PER_CHANNEL * OUTPUT_CHANNEL_COUNT * _outputFormat.sampleRate()) /
        _desiredOutputFormat.sampleRate();
    _receivedAudioStream.outputFormatChanged(_outputFormat.sampleRate(), OUTPUT_CHANNEL_COUNT);
}

bool AudioClient::switchInputToAudioDevice(const QAudioDeviceInfo& inputDeviceInfo) {
    bool supportedFormat = false;

    // cleanup any previously initialized device
    if (_audioInput) {
        // The call to stop() causes _inputDevice to be destructed.
        // That in turn causes it to be disconnected (see for example
        // http://stackoverflow.com/questions/9264750/qt-signals-and-slots-object-disconnect).
        _audioInput->stop();
        _inputDevice = NULL;

        delete _audioInput;
        _audioInput = NULL;
        _numInputCallbackBytes = 0;

        _inputAudioDeviceName = "";
    }

    if (_inputToNetworkResampler) {
        // if we were using an input to network resampler, delete it here
        delete _inputToNetworkResampler;
        _inputToNetworkResampler = NULL;
    }

    if (!inputDeviceInfo.isNull()) {
        qCDebug(audioclient) << "The audio input device " << inputDeviceInfo.deviceName() << "is available.";
        _inputAudioDeviceName = inputDeviceInfo.deviceName().trimmed();

        if (adjustedFormatForAudioDevice(inputDeviceInfo, _desiredInputFormat, _inputFormat)) {
            qCDebug(audioclient) << "The format to be used for audio input is" << _inputFormat;

            // we've got the best we can get for input
            // if required, setup a resampler for this input to our desired network format
            if (_inputFormat != _desiredInputFormat
                && _inputFormat.sampleRate() != _desiredInputFormat.sampleRate()) {
                qCDebug(audioclient) << "Attemping to create a resampler for input format to network format.";

                assert(_inputFormat.sampleSize() == 16);
                assert(_desiredInputFormat.sampleSize() == 16);
                int channelCount = (_inputFormat.channelCount() == 2 && _desiredInputFormat.channelCount() == 2) ? 2 : 1;

                _inputToNetworkResampler = new AudioSRC(_inputFormat.sampleRate(), _desiredInputFormat.sampleRate(), channelCount);

            } else {
                qCDebug(audioclient) << "No resampling required for audio input to match desired network format.";
            }

            // if the user wants stereo but this device can't provide then bail
            if (!_isStereoInput || _inputFormat.channelCount() == 2) {
                _audioInput = new QAudioInput(inputDeviceInfo, _inputFormat, this);
                _numInputCallbackBytes = calculateNumberOfInputCallbackBytes(_inputFormat);
                _audioInput->setBufferSize(_numInputCallbackBytes);

                // how do we want to handle input working, but output not working?
                int numFrameSamples = calculateNumberOfFrameSamples(_numInputCallbackBytes);
                _inputRingBuffer.resizeForFrameSize(numFrameSamples);

                // NOTE: device start() uses the Qt internal device list
                Lock lock(_deviceMutex);
                _inputDevice = _audioInput->start();
                lock.unlock();

                if (_inputDevice) {
                    connect(_inputDevice, SIGNAL(readyRead()), this, SLOT(handleMicAudioInput()));
                    supportedFormat = true;
                } else {
                    qCDebug(audioclient) << "Error starting audio input -" <<  _audioInput->error();
                }
            }
        }
    }

    return supportedFormat;
}

void AudioClient::outputNotify() {
    int recentUnfulfilled = _audioOutputIODevice.getRecentUnfulfilledReads();
    if (recentUnfulfilled > 0) {
        qCDebug(audioclient, "Starve detected, %d new unfulfilled reads", recentUnfulfilled);

        if (_outputStarveDetectionEnabled.get()) {
            quint64 now = usecTimestampNow() / 1000;
            int dt = (int)(now - _outputStarveDetectionStartTimeMsec);
            if (dt > STARVE_DETECTION_PERIOD) {
                _outputStarveDetectionStartTimeMsec = now;
                _outputStarveDetectionCount = 0;
            } else {
                _outputStarveDetectionCount += recentUnfulfilled;
                if (_outputStarveDetectionCount > STARVE_DETECTION_THRESHOLD) {
                    int oldOutputBufferSizeFrames = _sessionOutputBufferSizeFrames;
                    int newOutputBufferSizeFrames = setOutputBufferSize(oldOutputBufferSizeFrames + 1, false);

                    if (newOutputBufferSizeFrames > oldOutputBufferSizeFrames) {
                        qCDebug(audioclient,
                                "Starve threshold surpassed (%d starves in %d ms)", _outputStarveDetectionCount, dt);
                    }

                    _outputStarveDetectionStartTimeMsec = now;
                    _outputStarveDetectionCount = 0;
                }
            }
        }
    }
}

bool AudioClient::switchOutputToAudioDevice(const QAudioDeviceInfo& outputDeviceInfo) {
    bool supportedFormat = false;

    RecursiveLock lock(_localAudioMutex);
    _localSamplesAvailable.exchange(0, std::memory_order_release);

    // cleanup any previously initialized device
    if (_audioOutput) {
        _audioOutput->stop();

        delete _audioOutput;
        _audioOutput = NULL;

        _loopbackOutputDevice = NULL;
        delete _loopbackAudioOutput;
        _loopbackAudioOutput = NULL;

        delete[] _outputMixBuffer;
        _outputMixBuffer = NULL;

        delete[] _outputScratchBuffer;
        _outputScratchBuffer = NULL;

        delete[] _localOutputMixBuffer;
        _localOutputMixBuffer = NULL;
    }

    if (_networkToOutputResampler) {
        // if we were using an input to network resampler, delete it here
        delete _networkToOutputResampler;
        _networkToOutputResampler = NULL;

        delete _localToOutputResampler;
        _localToOutputResampler = NULL;
    }

    if (!outputDeviceInfo.isNull()) {
        qCDebug(audioclient) << "The audio output device " << outputDeviceInfo.deviceName() << "is available.";
        _outputAudioDeviceName = outputDeviceInfo.deviceName().trimmed();

        if (adjustedFormatForAudioDevice(outputDeviceInfo, _desiredOutputFormat, _outputFormat)) {
            qCDebug(audioclient) << "The format to be used for audio output is" << _outputFormat;

            // we've got the best we can get for input
            // if required, setup a resampler for this input to our desired network format
            if (_desiredOutputFormat != _outputFormat
                && _desiredOutputFormat.sampleRate() != _outputFormat.sampleRate()) {
                qCDebug(audioclient) << "Attemping to create a resampler for network format to output format.";

                assert(_desiredOutputFormat.sampleSize() == 16);
                assert(_outputFormat.sampleSize() == 16);

                _networkToOutputResampler = new AudioSRC(_desiredOutputFormat.sampleRate(), _outputFormat.sampleRate(), OUTPUT_CHANNEL_COUNT);
                _localToOutputResampler = new AudioSRC(_desiredOutputFormat.sampleRate(), _outputFormat.sampleRate(), OUTPUT_CHANNEL_COUNT);

            } else {
                qCDebug(audioclient) << "No resampling required for network output to match actual output format.";
            }

            outputFormatChanged();

            // setup our general output device for audio-mixer audio
            _audioOutput = new QAudioOutput(outputDeviceInfo, _outputFormat, this);

            int deviceChannelCount = _outputFormat.channelCount();
            int frameSize = (AudioConstants::NETWORK_FRAME_SAMPLES_PER_CHANNEL * deviceChannelCount * _outputFormat.sampleRate()) / _desiredOutputFormat.sampleRate();
            int requestedSize = _sessionOutputBufferSizeFrames * frameSize * AudioConstants::SAMPLE_SIZE;
            _audioOutput->setBufferSize(requestedSize);

            // initialize mix buffers on the _audioOutput thread to avoid races
            connect(_audioOutput, &QAudioOutput::stateChanged, [&, frameSize, requestedSize](QAudio::State state) {
                if (state == QAudio::ActiveState) {
                    // restrict device callback to _outputPeriod samples
                    _outputPeriod = (_audioOutput->periodSize() / AudioConstants::SAMPLE_SIZE) * 2;
                    _outputMixBuffer = new float[_outputPeriod];
                    _outputScratchBuffer = new int16_t[_outputPeriod];

                    // size local output mix buffer based on resampled network frame size
                    _networkPeriod = _localToOutputResampler->getMaxOutput(AudioConstants::NETWORK_FRAME_SAMPLES_STEREO);
                    _localOutputMixBuffer = new float[_networkPeriod];
                    int localPeriod = _outputPeriod * 2;
                    _localInjectorsStream.resizeForFrameSize(localPeriod);

                    int bufferSize = _audioOutput->bufferSize();
                    int bufferSamples = bufferSize / AudioConstants::SAMPLE_SIZE;
                    int bufferFrames = bufferSamples / (float)frameSize;
                    qCDebug(audioclient) << "frame (samples):" << frameSize;
                    qCDebug(audioclient) << "buffer (frames):" << bufferFrames;
                    qCDebug(audioclient) << "buffer (samples):" << bufferSamples;
                    qCDebug(audioclient) << "buffer (bytes):" << bufferSize;
                    qCDebug(audioclient) << "requested (bytes):" << requestedSize;
                    qCDebug(audioclient) << "period (samples):" << _outputPeriod;
                    qCDebug(audioclient) << "local buffer (samples):" << localPeriod;

                    disconnect(_audioOutput, &QAudioOutput::stateChanged, 0, 0);
                }
            });
            connect(_audioOutput, &QAudioOutput::notify, this, &AudioClient::outputNotify);

            _audioOutputIODevice.start();

            // NOTE: device start() uses the Qt internal device list
            Lock lock(_deviceMutex);
            _audioOutput->start(&_audioOutputIODevice);
            lock.unlock();

            // setup a loopback audio output device
            _loopbackAudioOutput = new QAudioOutput(outputDeviceInfo, _outputFormat, this);

            _timeSinceLastReceived.start();

            supportedFormat = true;
        }
    }

    return supportedFormat;
}

int AudioClient::setOutputBufferSize(int numFrames, bool persist) {
    numFrames = std::min(std::max(numFrames, MIN_BUFFER_FRAMES), MAX_BUFFER_FRAMES);
    if (numFrames != _sessionOutputBufferSizeFrames) {
        qCInfo(audioclient, "Audio output buffer set to %d frames", numFrames);
        _sessionOutputBufferSizeFrames = numFrames;
        if (persist) {
            _outputBufferSizeFrames.set(numFrames);
        }

        if (_audioOutput) {
            // The buffer size can't be adjusted after QAudioOutput::start() has been called, so
            // recreate the device by switching to the default.
            QAudioDeviceInfo outputDeviceInfo = defaultAudioDeviceForMode(QAudio::AudioOutput);
            emit changeDevice(outputDeviceInfo);  // On correct thread, please, as setOutputBufferSize can be called from main thread.
        }
    }
    return numFrames;
}

// The following constant is operating system dependent due to differences in
// the way input audio is handled. The audio input buffer size is inversely
// proportional to the accelerator ratio.

#ifdef Q_OS_WIN
const float AudioClient::CALLBACK_ACCELERATOR_RATIO = IsWindows8OrGreater() ? 1.0f : 0.25f;
#endif

#ifdef Q_OS_MAC
const float AudioClient::CALLBACK_ACCELERATOR_RATIO = 2.0f;
#endif

#ifdef Q_OS_LINUX
const float AudioClient::CALLBACK_ACCELERATOR_RATIO = 2.0f;
#endif

int AudioClient::calculateNumberOfInputCallbackBytes(const QAudioFormat& format) const {
    int numInputCallbackBytes = (int)(((AudioConstants::NETWORK_FRAME_BYTES_PER_CHANNEL
        * format.channelCount()
        * ((float) format.sampleRate() / AudioConstants::SAMPLE_RATE))
        / CALLBACK_ACCELERATOR_RATIO) + 0.5f);

    return numInputCallbackBytes;
}

int AudioClient::calculateNumberOfFrameSamples(int numBytes) const {
    int frameSamples = (int)(numBytes * CALLBACK_ACCELERATOR_RATIO + 0.5f) / AudioConstants::SAMPLE_SIZE;
    return frameSamples;
}

float AudioClient::azimuthForSource(const glm::vec3& relativePosition) {
    // copied from AudioMixer, more or less
    glm::quat inverseOrientation = glm::inverse(_orientationGetter());
    
    // compute sample delay for the 2 ears to create phase panning
    glm::vec3 rotatedSourcePosition = inverseOrientation * relativePosition;
    
    // project the rotated source position vector onto x-y plane
    rotatedSourcePosition.y = 0.0f;

    static const float SOURCE_DISTANCE_THRESHOLD = 1e-30f;

    if (glm::length2(rotatedSourcePosition) > SOURCE_DISTANCE_THRESHOLD) {
        
        // produce an oriented angle about the y-axis
        return glm::orientedAngle(glm::vec3(0.0f, 0.0f, -1.0f), glm::normalize(rotatedSourcePosition), glm::vec3(0.0f, -1.0f, 0.0f));
    } else {
        
        // no azimuth if they are in same spot
        return 0.0f; 
    }
}

float AudioClient::gainForSource(float distance, float volume) {

    const float ATTENUATION_BEGINS_AT_DISTANCE = 1.0f;

    // I'm assuming that the AudioMixer's getting of the stream's attenuation
    // factor is basically same as getting volume
    float gain = volume;

    // attenuate based on distance
    if (distance >= ATTENUATION_BEGINS_AT_DISTANCE) {
        gain /= (distance/ATTENUATION_BEGINS_AT_DISTANCE);  // attenuation = -6dB * log2(distance)
    }

    return gain;
}

qint64 AudioClient::AudioOutputIODevice::readData(char * data, qint64 maxSize) {

    // samples requested from OUTPUT_CHANNEL_COUNT
    int deviceChannelCount = _audio->_outputFormat.channelCount();
    int samplesRequested = (int)(maxSize / AudioConstants::SAMPLE_SIZE) * OUTPUT_CHANNEL_COUNT / deviceChannelCount;
    // restrict samplesRequested to the size of our mix/scratch buffers
    samplesRequested = std::min(samplesRequested, _audio->_outputPeriod);

    int16_t* scratchBuffer = _audio->_outputScratchBuffer;
    float* mixBuffer = _audio->_outputMixBuffer;

    int networkSamplesPopped;
    if ((networkSamplesPopped = _receivedAudioStream.popSamples(samplesRequested, false)) > 0) {
        qCDebug(audiostream, "Read %d samples from buffer (%d available, %d requested)", networkSamplesPopped, _receivedAudioStream.getSamplesAvailable(), samplesRequested);
        AudioRingBuffer::ConstIterator lastPopOutput = _receivedAudioStream.getLastPopOutput();
        lastPopOutput.readSamples(scratchBuffer, networkSamplesPopped);

        for (int i = 0; i < networkSamplesPopped; i++) {
            mixBuffer[i] = convertToFloat(scratchBuffer[i]);
        }

        samplesRequested = networkSamplesPopped;
    }

    int injectorSamplesPopped = 0;
    {
        RecursiveLock lock(_audio->_localAudioMutex);
        bool append = networkSamplesPopped > 0;
        samplesRequested = std::min(samplesRequested, _audio->_localSamplesAvailable.load(std::memory_order_acquire));
        if ((injectorSamplesPopped = _localInjectorsStream.appendSamples(mixBuffer, samplesRequested, append)) > 0) {
            _audio->_localSamplesAvailable.fetch_sub(injectorSamplesPopped, std::memory_order_release);
            qCDebug(audiostream, "Read %d samples from injectors (%d available, %d requested)", injectorSamplesPopped, _localInjectorsStream.samplesAvailable(), samplesRequested);
        }
    }

    // prepare injectors for the next callback
    QMetaObject::invokeMethod(&_audio->_localAudioThread, "prepare", Qt::QueuedConnection);

    int samplesPopped = std::max(networkSamplesPopped, injectorSamplesPopped);
    int framesPopped = samplesPopped / AudioConstants::STEREO;
    int bytesWritten;
    if (samplesPopped > 0) {
        if (deviceChannelCount == OUTPUT_CHANNEL_COUNT) {
            // limit the audio
            _audio->_audioLimiter.render(mixBuffer, (int16_t*)data, framesPopped);
        } else {
            _audio->_audioLimiter.render(mixBuffer, scratchBuffer, framesPopped);

            // upmix or downmix to deviceChannelCount
            if (deviceChannelCount > OUTPUT_CHANNEL_COUNT) {
                int extraChannels = deviceChannelCount - OUTPUT_CHANNEL_COUNT;
                channelUpmix(scratchBuffer, (int16_t*)data, samplesPopped, extraChannels);
            } else {
                channelDownmix(scratchBuffer, (int16_t*)data, samplesPopped);
            }
        }

        bytesWritten = framesPopped * AudioConstants::SAMPLE_SIZE * deviceChannelCount;
    } else {
        // nothing on network, don't grab anything from injectors, and just return 0s
        memset(data, 0, maxSize);
        bytesWritten = maxSize;
    }

    int bytesAudioOutputUnplayed = _audio->_audioOutput->bufferSize() - _audio->_audioOutput->bytesFree();
    float msecsAudioOutputUnplayed = bytesAudioOutputUnplayed / (float)_audio->_outputFormat.bytesForDuration(USECS_PER_MSEC);
    _audio->_stats.updateOutputMsUnplayed(msecsAudioOutputUnplayed);

    if (bytesAudioOutputUnplayed == 0) {
        _unfulfilledReads++;
    }

    return bytesWritten;
}

// now called from a background thread, to keep blocking operations off the audio thread
void AudioClient::checkDevices() {
    QVector<QString> inputDevices = getDeviceNames(QAudio::AudioInput);
    QVector<QString> outputDevices = getDeviceNames(QAudio::AudioOutput);

    if (inputDevices != _inputDevices || outputDevices != _outputDevices) {
        _inputDevices = inputDevices;
        _outputDevices = outputDevices;

        emit deviceChanged();
    }
}

void AudioClient::loadSettings() {
    _receivedAudioStream.setDynamicJitterBufferEnabled(dynamicJitterBufferEnabled.get());
    _receivedAudioStream.setStaticJitterBufferFrames(staticJitterBufferFrames.get());

    qCDebug(audioclient) << "---- Initializing Audio Client ----";
    auto codecPlugins = PluginManager::getInstance()->getCodecPlugins();
    for (auto& plugin : codecPlugins) {
        qCDebug(audioclient) << "Codec available:" << plugin->getName();
    }

}

void AudioClient::saveSettings() {
    dynamicJitterBufferEnabled.set(_receivedAudioStream.dynamicJitterBufferEnabled());
    staticJitterBufferFrames.set(_receivedAudioStream.getStaticJitterBufferFrames());
}

void AudioClient::setAvatarBoundingBoxParameters(glm::vec3 corner, glm::vec3 scale) {
    avatarBoundingBoxCorner = corner;
    avatarBoundingBoxScale = scale;
}
