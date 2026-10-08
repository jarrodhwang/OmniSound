// OmniSound virtual render-only endpoint. Adapted from the MIT-licensed SysVAD tables.
#pragma once
#include "mintopo.h"
#include "minwavert.h"

NTSTATUS CreateMiniportTopologySYSVAD(PUNKNOWN*, REFCLSID, PUNKNOWN, POOL_FLAGS, PUNKNOWN, PVOID, PENDPOINT_MINIPAIR);
NTSTATUS CreateMiniportWaveRTSYSVAD(PUNKNOWN*, REFCLSID, PUNKNOWN, POOL_FLAGS, PUNKNOWN, PVOID, PENDPOINT_MINIPAIR);

#define OMNI_PCM_FORMAT(channels, bits, mask) \
    {{sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE),0,0,0,STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)}, \
     {{WAVE_FORMAT_EXTENSIBLE,channels,48000,48000*channels*(bits/8),channels*(bits/8),bits,22},bits,mask,STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM)}}
static KSDATAFORMAT_WAVEFORMATEXTENSIBLE OmniFormats[] = {
    OMNI_PCM_FORMAT(2,32,KSAUDIO_SPEAKER_STEREO),
    OMNI_PCM_FORMAT(2,24,KSAUDIO_SPEAKER_STEREO),
    OMNI_PCM_FORMAT(2,16,KSAUDIO_SPEAKER_STEREO),
    OMNI_PCM_FORMAT(6,32,KSAUDIO_SPEAKER_5POINT1_SURROUND),
    OMNI_PCM_FORMAT(6,24,KSAUDIO_SPEAKER_5POINT1_SURROUND),
    OMNI_PCM_FORMAT(6,16,KSAUDIO_SPEAKER_5POINT1_SURROUND),
    OMNI_PCM_FORMAT(8,32,KSAUDIO_SPEAKER_7POINT1_SURROUND),
    OMNI_PCM_FORMAT(8,24,KSAUDIO_SPEAKER_7POINT1_SURROUND),
    OMNI_PCM_FORMAT(8,16,KSAUDIO_SPEAKER_7POINT1_SURROUND)
};
#undef OMNI_PCM_FORMAT
static MODE_AND_DEFAULT_FORMAT OmniModes[] = {
    {STATIC_AUDIO_SIGNALPROCESSINGMODE_DEFAULT,&OmniFormats[0].DataFormat},
    {STATIC_AUDIO_SIGNALPROCESSINGMODE_RAW,&OmniFormats[0].DataFormat}
};
static PIN_DEVICE_FORMATS_AND_MODES OmniPinFormats[] = {
    {SystemRenderPin,OmniFormats,SIZEOF_ARRAY(OmniFormats),OmniModes,SIZEOF_ARRAY(OmniModes)},
    {BridgePin,nullptr,0,nullptr,0}
};
static KSDATARANGE_AUDIO OmniStreamRange = {
    {sizeof(KSDATARANGE_AUDIO),0,0,0,STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)},8,16,32,48000,48000
};
static KSDATARANGE OmniBridgeRange = {
    sizeof(KSDATARANGE),0,0,0,STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
};
static PKSDATARANGE OmniStreamRanges[] = {reinterpret_cast<PKSDATARANGE>(&OmniStreamRange)};
static PKSDATARANGE OmniBridgeRanges[] = {&OmniBridgeRange};
static PCPIN_DESCRIPTOR OmniWavePins[] = {
    {1,1,0,nullptr,{0,nullptr,0,nullptr,1,OmniStreamRanges,KSPIN_DATAFLOW_IN,KSPIN_COMMUNICATION_SINK,&KSCATEGORY_AUDIO,nullptr,0}},
    {0,0,0,nullptr,{0,nullptr,0,nullptr,1,OmniBridgeRanges,KSPIN_DATAFLOW_OUT,KSPIN_COMMUNICATION_NONE,&KSCATEGORY_AUDIO,nullptr,0}}
};
static PCPROPERTY_ITEM OmniWaveProperties[] = {
    {&KSPROPSETID_Pin,KSPROPERTY_PIN_PROPOSEDATAFORMAT,KSPROPERTY_TYPE_SET|KSPROPERTY_TYPE_BASICSUPPORT,PropertyHandler_WaveFilter},
    {&KSPROPSETID_Pin,KSPROPERTY_PIN_PROPOSEDATAFORMAT2,KSPROPERTY_TYPE_GET|KSPROPERTY_TYPE_BASICSUPPORT,PropertyHandler_WaveFilter}
};
DEFINE_PCAUTOMATION_TABLE_PROP(OmniWaveAutomation,OmniWaveProperties);
static PCCONNECTION_DESCRIPTOR OmniWaveConnections[] = {{PCFILTER_NODE,0,PCFILTER_NODE,1}};
static PCFILTER_DESCRIPTOR OmniWaveDescriptor = {
    0,&OmniWaveAutomation,sizeof(PCPIN_DESCRIPTOR),SIZEOF_ARRAY(OmniWavePins),OmniWavePins,
    0,0,nullptr,SIZEOF_ARRAY(OmniWaveConnections),OmniWaveConnections,0,nullptr
};
// No hardware volume or fake hardware loopback: the Windows audio engine owns both.
static PCPIN_DESCRIPTOR OmniTopoPins[] = {
    {0,0,0,nullptr,{0,nullptr,0,nullptr,1,OmniBridgeRanges,KSPIN_DATAFLOW_IN,KSPIN_COMMUNICATION_NONE,&KSCATEGORY_AUDIO,nullptr,0}},
    {0,0,0,nullptr,{0,nullptr,0,nullptr,1,OmniBridgeRanges,KSPIN_DATAFLOW_OUT,KSPIN_COMMUNICATION_NONE,&KSNODETYPE_SPEAKER,nullptr,0}}
};
static PCCONNECTION_DESCRIPTOR OmniTopoConnections[] = {{PCFILTER_NODE,0,PCFILTER_NODE,1}};
static PCFILTER_DESCRIPTOR OmniTopoDescriptor = {
    0,nullptr,sizeof(PCPIN_DESCRIPTOR),SIZEOF_ARRAY(OmniTopoPins),OmniTopoPins,
    0,0,nullptr,SIZEOF_ARRAY(OmniTopoConnections),OmniTopoConnections,0,nullptr
};
static PHYSICALCONNECTIONTABLE OmniPhysicalConnections[] = {{0,1,CONNECTIONTYPE_WAVE_OUTPUT}};
static ENDPOINT_MINIPAIR OmniMiniports = {
    eSpeakerDevice,L"TopologyOmniSound",nullptr,CreateMiniportTopologySYSVAD,&OmniTopoDescriptor,0,nullptr,
    L"WaveOmniSound",nullptr,CreateMiniportWaveRTSYSVAD,&OmniWaveDescriptor,0,nullptr,
    8,OmniPinFormats,SIZEOF_ARRAY(OmniPinFormats),OmniPhysicalConnections,SIZEOF_ARRAY(OmniPhysicalConnections),
    ENDPOINT_NO_FLAGS,nullptr,0,nullptr
};
static PENDPOINT_MINIPAIR g_RenderEndpoints[] = {&OmniMiniports};
static PENDPOINT_MINIPAIR g_CaptureEndpoints[] = {nullptr};
#define g_cRenderEndpoints 1
#define g_cCaptureEndpoints 0
#define g_MaxMiniports 2
