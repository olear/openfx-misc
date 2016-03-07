 /* ***** BEGIN LICENSE BLOCK *****
 * This file is part of openfx-misc <https://github.com/devernay/openfx-misc>,
 * Copyright (C) 2015 INRIA
 *
 * openfx-misc is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * openfx-misc is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with openfx-misc.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

/*
 * OFX Shuffle plugin.
 */

#include <cmath>
#include <set>
#include <algorithm>
#ifdef _WINDOWS
#include <windows.h>
#endif

#include "ofxsProcessing.H"
#include "ofxsPixelProcessor.h"
#include "ofxsMacros.h"
#include "ofxsCoords.h"
#include "ofxsMultiPlane.h"

using namespace OFX;

OFXS_NAMESPACE_ANONYMOUS_ENTER

#define kPluginName "ShuffleOFX"
#define kPluginGrouping "Channel"
#define kPluginDescription "Rearrange channels from one or two inputs and/or convert to different bit depth or components. No colorspace conversion is done (mapping is linear, even for 8-bit and 16-bit types)."
#define kPluginIdentifier "net.sf.openfx.ShufflePlugin"
#define kPluginVersionMajor 2 // Incrementing this number means that you have broken backwards compatibility of the plug-in.
#define kPluginVersionMinor 0 // Increment this when you have fixed a bug or made it faster.

#define kSupportsTiles 1
#define kSupportsMultiResolution 1
#define kSupportsRenderScale 1
#define kSupportsMultipleClipPARs false
#define kSupportsMultipleClipDepths true // can convert depth
#define kRenderThreadSafety eRenderFullySafe

#define kEnableMultiPlanar true

#define kParamOutputComponents "outputComponents"
#define kParamOutputComponentsLabel "Output Components"
#define kParamOutputComponentsHint "Select what types of components the plug-in should output, this has an effect only when the Output Layer is set to the Color layer." \
" This controls what should be the components for the Color Layer: Alpha, RGB or RGBA"
#define kParamOutputComponentsOptionRGBA "RGBA"
#define kParamOutputComponentsOptionRGB "RGB"
#define kParamOutputComponentsOptionAlpha "Alpha"

#define kParamOutputChannels kNatronOfxParamOutputChannels
#define kParamOutputChannelsChoice kParamOutputChannels "Choice"
#define kParamOutputChannelsLabel "Output Layer"
#define kParamOutputChannelsHint "The layer that will be written to in output"


#define kParamOutputBitDepth "outputBitDepth"
#define kParamOutputBitDepthLabel "Output Bit Depth"
#define kParamOutputBitDepthHint "Bit depth of the output.\nWARNING: the conversion is linear, even for 8-bit or 16-bit depth. Use with care."
#define kParamOutputBitDepthOptionByte "Byte (8 bits)"
#define kParamOutputBitDepthOptionShort "Short (16 bits)"
#define kParamOutputBitDepthOptionFloat "Float (32 bits)"

#define kParamOutputPremultiplication "outputPremult"
#define kParamOutputPremultiplicationLabel "Premultiplication Metadata"
#define kParamOutputPremultiplicationHint "Set the premultiplication meta-data that will flow down-stream so that further down effects " \
"know what kind of data to expect. By default it should be set to Unpremultiplied and you should always provide the Shuffle node " \
"unpremultiplied data. Providing alpha-premultiplied data in input of the Shuffle may produce wrong results because of the potential loss " \
"of the associated alpha channel."


#define kParamOutputR "outputR"
#define kParamOutputRChoice kParamOutputR "Choice"
#define kParamOutputRLabel "R"
#define kParamOutputRHint "Input channel for the output red channel"

#define kParamOutputG "outputG"
#define kParamOutputGChoice kParamOutputG "Choice"
#define kParamOutputGLabel "G"
#define kParamOutputGHint "Input channel for the output green channel"

#define kParamOutputB "outputB"
#define kParamOutputBChoice kParamOutputB "Choice"
#define kParamOutputBLabel "B"
#define kParamOutputBHint "Input channel for the output blue channel"


#define kParamOutputA "outputA"
#define kParamOutputAChoice kParamOutputA "Choice"
#define kParamOutputALabel "A"
#define kParamOutputAHint "Input channel for the output alpha channel"



#define kParamClipInfo "clipInfo"
#define kParamClipInfoLabel "Clip Info..."
#define kParamClipInfoHint "Display information about the inputs"

// TODO: sRGB/Rec.709 conversions for byte/short types

enum InputChannelEnum {
    eInputChannelAR = 0,
    eInputChannelAG,
    eInputChannelAB,
    eInputChannelAA,
    eInputChannel0,
    eInputChannel1,
    eInputChannelBR,
    eInputChannelBG,
    eInputChannelBB,
    eInputChannelBA,
};

#define kClipA "A"
#define kClipB "B"

static bool gSupportsBytes  = false;
static bool gSupportsShorts = false;
static bool gSupportsFloats = false;
static bool gSupportsRGBA   = false;
static bool gSupportsRGB    = false;
static bool gSupportsAlpha  = false;
static bool gSupportsDynamicChoices = false;
static bool gIsMultiPlanar = false;

static OFX::PixelComponentEnum gOutputComponentsMap[4]; // 3 components + a sentinel at the end with ePixelComponentNone
static OFX::BitDepthEnum gOutputBitDepthMap[4]; // 3 possible bit depths + a sentinel


class ShufflerBase : public OFX::ImageProcessor
{
protected:
    const OFX::Image *_srcImgA;
    const OFX::Image *_srcImgB;
    PixelComponentEnum _outputLayer;
    int _outputComponentCount;
    BitDepthEnum _outputBitDepth;
    std::vector<InputChannelEnum> _channelMap;

    public:
    ShufflerBase(OFX::ImageEffect &instance)
    : OFX::ImageProcessor(instance)
    , _srcImgA(0)
    , _srcImgB(0)
    , _outputLayer(ePixelComponentNone)
    , _outputComponentCount(0)
    , _outputBitDepth(eBitDepthNone)
    , _channelMap()
    {
    }

    void setSrcImg(const OFX::Image *A, const OFX::Image *B) {_srcImgA = A; _srcImgB = B;}

    void setValues(PixelComponentEnum outputComponents,
                   int outputComponentCount,
                   BitDepthEnum outputBitDepth,
                   const std::vector<InputChannelEnum> &channelMap)
    {
        _outputLayer = outputComponents,
        _outputComponentCount = outputComponentCount,
        _outputBitDepth = outputBitDepth;
        assert(_outputComponentCount == (int)channelMap.size());
        _channelMap = channelMap;
    }
};

//////////////////////////////
// PIXEL CONVERSION ROUTINES

/// maps 0-(numvals-1) to 0.-1.
template<int numvals>
static float intToFloat(int value)
{
    return value / (float)(numvals-1);
}

/// maps °.-1. to 0-(numvals-1)
template<int numvals>
static int floatToInt(float value)
{
    if (value <= 0) {
        return 0;
    } else if (value >= 1.) {
        return numvals - 1;
    }
    return value * (numvals-1) + 0.5f;
}

template <typename SRCPIX,typename DSTPIX>
static DSTPIX convertPixelDepth(SRCPIX pix);

///explicit template instantiations

template <> float convertPixelDepth(unsigned char pix)
{
    return intToFloat<65536>(pix);
}

template <> unsigned short convertPixelDepth(unsigned char pix)
{
    // 0x01 -> 0x0101, 0x02 -> 0x0202, ..., 0xff -> 0xffff
    return (unsigned short)((pix << 8) + pix);
}

template <> unsigned char convertPixelDepth(unsigned char pix)
{
    return pix;
}

template <> unsigned char convertPixelDepth(unsigned short pix)
{
    // the following is from ImageMagick's quantum.h
    return (unsigned char)(((pix+128UL)-((pix+128UL) >> 8)) >> 8);
}

template <> float convertPixelDepth(unsigned short pix)
{
    return intToFloat<65536>(pix);
}

template <> unsigned short convertPixelDepth(unsigned short pix)
{
    return pix;
}

template <> unsigned char convertPixelDepth(float pix)
{
    return (unsigned char)floatToInt<256>(pix);
}

template <> unsigned short convertPixelDepth(float pix)
{
    return (unsigned short)floatToInt<65536>(pix);
}

template <> float convertPixelDepth(float pix)
{
    return pix;
}


template <class PIXSRC, class PIXDST, int nComponentsDst>
class Shuffler : public ShufflerBase
{
public:
    Shuffler(OFX::ImageEffect &instance)
    : ShufflerBase(instance)
    {
    }

private:
    void multiThreadProcessImages(OfxRectI procWindow)
    {
        const OFX::Image* channelMapImg[nComponentsDst];
        int channelMapComp[nComponentsDst]; // channel component, or value if no image
        int srcMapComp[4]; // R,G,B,A components for src
        PixelComponentEnum srcComponents = ePixelComponentNone;
        if (_srcImgA) {
            srcComponents = _srcImgA->getPixelComponents();
        } else if (_srcImgB) {
            srcComponents = _srcImgB->getPixelComponents();
        }
        switch (srcComponents) {
            case OFX::ePixelComponentRGBA:
                srcMapComp[0] = 0;
                srcMapComp[1] = 1;
                srcMapComp[2] = 2;
                srcMapComp[3] = 3;
                break;
            case OFX::ePixelComponentRGB:
                srcMapComp[0] = 0;
                srcMapComp[1] = 1;
                srcMapComp[2] = 2;
                srcMapComp[3] = -1;
                break;
            case OFX::ePixelComponentAlpha:
                srcMapComp[0] = -1;
                srcMapComp[1] = -1;
                srcMapComp[2] = -1;
                srcMapComp[3] = 0;
                break;
#ifdef OFX_EXTENSIONS_NATRON
            case OFX::ePixelComponentXY:
                srcMapComp[0] = 0;
                srcMapComp[1] = 1;
                srcMapComp[2] = -1;
                srcMapComp[3] = -1;
                break;
#endif
            default:
                srcMapComp[0] = -1;
                srcMapComp[1] = -1;
                srcMapComp[2] = -1;
                srcMapComp[3] = -1;
                break;
        }
        for (int c = 0; c < nComponentsDst; ++c) {
            channelMapImg[c] = NULL;
            channelMapComp[c] = 0;
            switch (_channelMap[c]) {
                case eInputChannelAR:
                    if (_srcImgA && srcMapComp[0] >= 0) {
                        channelMapImg[c] = _srcImgA;
                        channelMapComp[c] = srcMapComp[0]; // srcImg may not have R!!!
                    }
                    break;
                case eInputChannelAG:
                    if (_srcImgA && srcMapComp[1] >= 0) {
                        channelMapImg[c] = _srcImgA;
                        channelMapComp[c] = srcMapComp[1];
                    }
                    break;
                case eInputChannelAB:
                    if (_srcImgA && srcMapComp[2] >= 0) {
                        channelMapImg[c] = _srcImgA;
                        channelMapComp[c] = srcMapComp[2];
                    }
                    break;
                case eInputChannelAA:
                    if (_srcImgA && srcMapComp[3] >= 0) {
                        channelMapImg[c] = _srcImgA;
                        channelMapComp[c] = srcMapComp[3];
                    }
                    break;
                case eInputChannel0:
                    channelMapComp[c] = 0;
                    break;
                case eInputChannel1:
                    channelMapComp[c] = 1;
                    break;
                case eInputChannelBR:
                    if (_srcImgB && srcMapComp[0] >= 0) {
                        channelMapImg[c] = _srcImgB;
                        channelMapComp[c] = srcMapComp[0];
                    }
                    break;
                case eInputChannelBG:
                    if (_srcImgB && srcMapComp[1] >= 0) {
                        channelMapImg[c] = _srcImgB;
                        channelMapComp[c] = srcMapComp[1];
                    }
                    break;
                case eInputChannelBB:
                    if (_srcImgB && srcMapComp[2] >= 0) {
                        channelMapImg[c] = _srcImgB;
                        channelMapComp[c] = srcMapComp[2];
                    }
                    break;
                case eInputChannelBA:
                    if (_srcImgB && srcMapComp[3] >= 0) {
                        channelMapImg[c] = _srcImgB;
                        channelMapComp[c] = srcMapComp[3];
                    }
                    break;
            }
        }
        // now compute the transformed image, component by component
        for (int c = 0; c < nComponentsDst; ++c) {
            const OFX::Image* srcImg = channelMapImg[c];
            int srcComp = channelMapComp[c];

            for (int y = procWindow.y1; y < procWindow.y2; y++) {
                if (_effect.abort()) {
                    break;
                }

                PIXDST *dstPix = (PIXDST *) _dstImg->getPixelAddress(procWindow.x1, y);

                for (int x = procWindow.x1; x < procWindow.x2; x++) {
                    PIXSRC *srcPix = (PIXSRC *)  (srcImg ? srcImg->getPixelAddress(x, y) : 0);
                    // if there is a srcImg but we are outside of its RoD, it should be considered black and transparent
                    dstPix[c] = srcImg ? convertPixelDepth<PIXSRC,PIXDST>(srcPix ? srcPix[srcComp] : 0) : convertPixelDepth<float,PIXDST>(srcComp);
                    dstPix += nComponentsDst;
                }
            }
        }
    }
};

struct InputPlaneChannel {
    OFX::Image* img;
    int channelIndex;
    bool fillZero;
    
    InputPlaneChannel() : img(0), channelIndex(-1), fillZero(true) {}
};

class MultiPlaneShufflerBase : public OFX::ImageProcessor
{
protected:
    
    int _outputComponentCount;
    BitDepthEnum _outputBitDepth;
    int _nComponentsDst;
    std::vector<InputPlaneChannel> _inputPlanes;

public:
    MultiPlaneShufflerBase(OFX::ImageEffect &instance)
    : OFX::ImageProcessor(instance)
    , _outputComponentCount(0)
    , _outputBitDepth(eBitDepthNone)
    , _nComponentsDst(0)
    , _inputPlanes(_nComponentsDst)
    {
    }

    void setValues(int outputComponentCount,
                   BitDepthEnum outputBitDepth,
                   const std::vector<InputPlaneChannel>& planes)
    {
        _outputComponentCount = outputComponentCount,
        _outputBitDepth = outputBitDepth;
        _inputPlanes = planes;

    }
};


template <class PIXSRC, class PIXDST, int nComponentsDst>
class MultiPlaneShuffler : public MultiPlaneShufflerBase
{
public:
    MultiPlaneShuffler(OFX::ImageEffect &instance)
    : MultiPlaneShufflerBase(instance)
    {
    }
    
private:
    void multiThreadProcessImages(OfxRectI procWindow)
    {
        assert(_inputPlanes.size() == nComponentsDst);
        // now compute the transformed image, component by component
        for (int c = 0; c < nComponentsDst; ++c) {
            
            const OFX::Image* srcImg = _inputPlanes[c].img;
            int srcComp = _inputPlanes[c].channelIndex;
            if (!srcImg) {
                srcComp = _inputPlanes[c].fillZero ? 0. : 1.;
            }
            
            for (int y = procWindow.y1; y < procWindow.y2; y++) {
                if (_effect.abort()) {
                    break;
                }
                
                PIXDST *dstPix = (PIXDST *) _dstImg->getPixelAddress(procWindow.x1, y);
                
                for (int x = procWindow.x1; x < procWindow.x2; x++) {
                    PIXSRC *srcPix = (PIXSRC *)  (srcImg ? srcImg->getPixelAddress(x, y) : 0);
                    // if there is a srcImg but we are outside of its RoD, it should be considered black and transparent
                    dstPix[c] = srcImg ? convertPixelDepth<PIXSRC,PIXDST>(srcPix ? srcPix[srcComp] : 0) : convertPixelDepth<float,PIXDST>(srcComp);
                    dstPix += nComponentsDst;
                }
            }
        }
    }
};


////////////////////////////////////////////////////////////////////////////////
/** @brief The plugin that does our work */
class ShufflePlugin : public OFX::ImageEffect
{
public:
    /** @brief ctor */
    ShufflePlugin(OfxImageEffectHandle handle, OFX::ContextEnum context)
    : ImageEffect(handle)
    , _dstClip(0)
    , _srcClipA(0)
    , _srcClipB(0)
    , _outputLayer(0)
    , _outputLayerString(0)
    , _outputBitDepth(0)
    , _channelParam()
    , _channelParamStrings()
    , _outputComponents(0)
    {
        _dstClip = fetchClip(kOfxImageEffectOutputClipName);
        assert(_dstClip && (1 <= _dstClip->getPixelComponentCount() && _dstClip->getPixelComponentCount() <= 4));
        _srcClipA = fetchClip(context == eContextGeneral ? kClipA : kOfxImageEffectSimpleSourceClipName);
        assert(_srcClipA && (1 <= _srcClipA->getPixelComponentCount() && _srcClipA->getPixelComponentCount() <= 4));
        if (context == eContextGeneral) {
            _srcClipB = fetchClip(kClipB);
            assert(_srcClipB && (1 <= _srcClipB->getPixelComponentCount() && _srcClipB->getPixelComponentCount() <= 4));
        }
        if (gIsMultiPlanar) {
            _outputLayer = fetchChoiceParam(kParamOutputChannels);
        }
        if (getImageEffectHostDescription()->supportsMultipleClipDepths) {
            _outputBitDepth = fetchChoiceParam(kParamOutputBitDepth);
        }
        _channelParam[0] = fetchChoiceParam(kParamOutputR);
        _channelParam[1] = fetchChoiceParam(kParamOutputG);
        _channelParam[2] = fetchChoiceParam(kParamOutputB);
        _channelParam[3] = fetchChoiceParam(kParamOutputA);
        
        _outputComponents = fetchChoiceParam(kParamOutputComponents);
        
        if (gSupportsDynamicChoices) {
            _outputLayerString = fetchStringParam(kParamOutputChannelsChoice);
            _channelParamStrings[0] = fetchStringParam(kParamOutputRChoice);
            _channelParamStrings[1] = fetchStringParam(kParamOutputGChoice);
            _channelParamStrings[2] = fetchStringParam(kParamOutputBChoice);
            _channelParamStrings[3] = fetchStringParam(kParamOutputAChoice);
            
            //We need to restore the choice params because the host may not call getClipPreference if all clips are disconnected
            //e.g: this can be from a copy/paste issued from the user
            std::list<OFX::MultiPlane::ChoiceStringParam> params;
            for (int i = 0; i < 4; ++i) {
                OFX::MultiPlane::ChoiceStringParam p;
                p.param = _channelParam[i];
                p.stringParam = _channelParamStrings[i];
                params.push_back(p);
            }
            OFX::MultiPlane::setChannelsFromStringParams(params, false);
        } else {
            _channelParamStrings[0] = _channelParamStrings[1] = _channelParamStrings[2] = _channelParamStrings[3] = 0;
        }
        
        //Refresh output components secretness
        std::string layerName;
        if (_outputLayerString) {
            _outputLayerString->getValue(layerName);
        }
        
        std::string ofxComponents;
        if (layerName.empty() ||
            layerName == kPlaneLabelColorRGBA ||
            layerName == kPlaneLabelColorRGB ||
            layerName == kPlaneLabelColorAlpha) {
            ofxComponents = _dstClip->getPixelComponentsProperty();
        }
        bool secret = true;
        if (ofxComponents == kOfxImageComponentAlpha || ofxComponents == kOfxImageComponentRGB || ofxComponents == kOfxImageComponentRGBA) {
            secret = false;
        }
        _outputComponents->setIsSecret(secret);
        
        _outputPremult = fetchChoiceParam(kParamOutputPremultiplication);
    }

private:
    
    void setChannelsFromRed(double time);
    
    /* Override the render */
    virtual void render(const OFX::RenderArguments &args) OVERRIDE FINAL;

    /* override is identity */
    virtual bool isIdentity(const OFX::IsIdentityArguments &args, OFX::Clip * &identityClip, double &identityTime) OVERRIDE FINAL;

    virtual bool getRegionOfDefinition(const OFX::RegionOfDefinitionArguments &args, OfxRectD &rod) OVERRIDE FINAL;
    
    virtual void getClipComponents(const OFX::ClipComponentsArguments& args, OFX::ClipComponentsSetter& clipComponents) OVERRIDE FINAL;

    /** @brief get the clip preferences */
    virtual void getClipPreferences(ClipPreferencesSetter &clipPreferences) OVERRIDE FINAL;

    /* override changedParam */
    virtual void changedParam(const OFX::InstanceChangedArgs &args, const std::string &paramName) OVERRIDE FINAL;

    /** @brief called when a clip has just been changed in some way (a rewire maybe) */
    virtual void changedClip(const InstanceChangedArgs &args, const std::string &clipName) OVERRIDE FINAL;

private:
    
    void setOutputComponentsParam(OFX::PixelComponentEnum comps);
    
    bool isIdentityInternal(double time, OFX::Clip*& identityClip);
    
    void enableComponents(OFX::PixelComponentEnum originalOutputComponents, OFX::PixelComponentEnum outputComponentsWithCreateAlpha);
    

    /* internal render function */
    template <class DSTPIX, int nComponentsDst>
    void renderInternalForDstBitDepth(const OFX::RenderArguments &args, OFX::BitDepthEnum srcBitDepth);

    template <int nComponentsDst>
    void renderInternal(const OFX::RenderArguments &args, OFX::BitDepthEnum srcBitDepth, OFX::BitDepthEnum dstBitDepth);

    /* set up and run a processor */
    void setupAndProcess(ShufflerBase &, const OFX::RenderArguments &args);
    void setupAndProcessMultiPlane(MultiPlaneShufflerBase &, const OFX::RenderArguments &args);


    // do not need to delete these, the ImageEffect is managing them for us
    OFX::Clip *_dstClip;
    OFX::Clip *_srcClipA;
    OFX::Clip *_srcClipB;

    OFX::ChoiceParam *_outputLayer;
    OFX::StringParam *_outputLayerString;
    OFX::ChoiceParam *_outputBitDepth;
    OFX::ChoiceParam* _channelParam[4];
    OFX::StringParam* _channelParamStrings[4];
    OFX::ChoiceParam *_outputComponents;
    OFX::ChoiceParam *_outputPremult;
    
    //Small cache only used on main-thread to speed up getclipPreferences
    std::list<std::string> _currentOutputComps;
    std::list<std::string> _currentCompsA;
    std::list<std::string> _currentCompsB;
};

void
ShufflePlugin::getClipComponents(const OFX::ClipComponentsArguments& args, OFX::ClipComponentsSetter& clipComponents)
{
    const double time = args.time;
    
    OFX::MultiPlane::PerClipComponentsMap perClipComps;
    OFX::MultiPlane::ClipsComponentsInfoBase& aInfo = perClipComps[kClipA];
    aInfo.clip = _srcClipA;
    aInfo.componentsPresent = _srcClipA->getComponentsPresent();
    
    OFX::MultiPlane::ClipsComponentsInfoBase& bInfo = perClipComps[kClipB];
    bInfo.clip = _srcClipB;
    bInfo.componentsPresent = _srcClipB->getComponentsPresent();
    
    if (gIsMultiPlanar) {
        std::list<std::string> outputComponents = _dstClip->getComponentsPresent();
        std::string ofxPlane,ofxComp;
        OFX::MultiPlane::getPlaneNeededInOutput(outputComponents, _dstClip, _outputLayer, &ofxPlane, &ofxComp);
        clipComponents.addClipComponents(*_dstClip, ofxComp);
    } else {
        PixelComponentEnum outputComponents = gOutputComponentsMap[_outputLayer->getValueAtTime(time)];
        clipComponents.addClipComponents(*_dstClip, outputComponents);
    }
    
    std::map<OFX::Clip*,std::set<std::string> > clipMap;
    
    
    bool isCreatingAlpha;
    for (int i = 0; i < 4; ++i) {
        std::string ofxComp,ofxPlane;
        int channelIndex;
        OFX::Clip* clip = 0;
        bool ok = OFX::MultiPlane::getPlaneNeededForParam(time, perClipComps, _channelParam[i], &clip, &ofxPlane, &ofxComp, &channelIndex, &isCreatingAlpha);
        if (!ok) {
            continue;
        }
        if (ofxComp == kMultiPlaneParamOutputOption0 || ofxComp == kMultiPlaneParamOutputOption1) {
            continue;
        }
        assert(clip);
        
        std::map<OFX::Clip*,std::set<std::string> >::iterator foundClip = clipMap.find(clip);
        if (foundClip == clipMap.end()) {
            std::set<std::string> s;
            s.insert(ofxComp);
            clipMap.insert(std::make_pair(clip, s));
            clipComponents.addClipComponents(*clip, ofxComp);
        } else {
            std::pair<std::set<std::string>::iterator,bool> ret = foundClip->second.insert(ofxComp);
            if (ret.second) {
                clipComponents.addClipComponents(*clip, ofxComp);
            }
        }
    }
    
   
}

struct IdentityChoiceData
{
    OFX::Clip* clip;
    std::string components;
    int index;
};

bool
ShufflePlugin::isIdentityInternal(double time, OFX::Clip*& identityClip)
{
    if (!gSupportsDynamicChoices || !gIsMultiPlanar) {
        InputChannelEnum r = InputChannelEnum(_channelParam[0]->getValueAtTime(time));
        InputChannelEnum g = InputChannelEnum(_channelParam[1]->getValueAtTime(time));
        InputChannelEnum b = InputChannelEnum(_channelParam[2]->getValueAtTime(time));
        InputChannelEnum a = InputChannelEnum(_channelParam[3]->getValueAtTime(time));
        
        if (r == eInputChannelAR && g == eInputChannelAG && b == eInputChannelAB && a == eInputChannelAA && _srcClipA) {
            identityClip = _srcClipA;
            
            return true;
        }
        if (r == eInputChannelBR && g == eInputChannelBG && b == eInputChannelBB && a == eInputChannelBA && _srcClipB) {
            identityClip = _srcClipB;
            
            return true;
        }
        return false;
    } else {
        OFX::MultiPlane::PerClipComponentsMap perClipComps;
        OFX::MultiPlane::ClipsComponentsInfoBase& aInfo = perClipComps[kClipA];
        aInfo.clip = _srcClipA;
        aInfo.componentsPresent = _srcClipA->getComponentsPresent();
        
        OFX::MultiPlane::ClipsComponentsInfoBase& bInfo = perClipComps[kClipB];
        bInfo.clip = _srcClipB;
        bInfo.componentsPresent = _srcClipB->getComponentsPresent();

        std::list<std::string> outputsComponents = _dstClip->getComponentsPresent();
        
        IdentityChoiceData data[4];
        
        
        std::string dstPlane,dstComponents;
        OFX::MultiPlane::getPlaneNeededInOutput(outputsComponents, _dstClip, _outputLayer, &dstPlane, &dstComponents);
        if (dstPlane != kFnOfxImagePlaneColour) {
            return false;
        }
        
        int expectedIndex = -1;
        for (int i = 0; i < 4; ++i) {
            std::string plane;
            bool isCreatingAlpha;
            bool ok = OFX::MultiPlane::getPlaneNeededForParam(time, perClipComps, _channelParam[i], &data[i].clip, &plane, &data[i].components, &data[i].index, &isCreatingAlpha);
            if (!ok) {
                //We might have an index in the param different from the actual components if getClipPreferences was not called so far
                return false;
            }
            if (plane != kFnOfxImagePlaneColour) {
                if (i != 3) {
                    //This is not the color plane, no identity
                    return false;
                } else {
                    //In this case if the A choice is visible, the user either checked "Create alpha" or he/she set it explicitly to 0 or 1
                    if (!_channelParam[3]->getIsSecret() &&! isCreatingAlpha) {
                        return false;
                    } else {
                        ///Do not do the checks below
                        continue;
                    }
                }
            }
            if (i > 0) {
                if (data[i].index != expectedIndex || data[i].components != data[0].components ||
                    data[i].clip != data[0].clip) {
                    return false;
                }
            }
            expectedIndex = data[i].index + 1;
        }
        identityClip = data[0].clip;
        return true;
    }
}

bool
ShufflePlugin::isIdentity(const OFX::IsIdentityArguments &args, OFX::Clip * &identityClip, double &/*identityTime*/)
{
    const double time = args.time;
    return isIdentityInternal(time, identityClip);
}

bool
ShufflePlugin::getRegionOfDefinition(const OFX::RegionOfDefinitionArguments &args, OfxRectD &rod)
{
    const double time = args.time;
    OFX::Clip* identityClip = 0;
    if (isIdentityInternal(time, identityClip)) {
        rod = identityClip->getRegionOfDefinition(args.time);
        return true;
    }
    if (_srcClipA && _srcClipA->isConnected() && _srcClipB && _srcClipB->isConnected()) {
        OfxRectD rodA = _srcClipA->getRegionOfDefinition(args.time);
        OfxRectD rodB = _srcClipB->getRegionOfDefinition(args.time);
        OFX::Coords::rectBoundingBox(rodA, rodB, &rod);

        return true;
    }
    return false;
}


////////////////////////////////////////////////////////////////////////////////
/** @brief render for the filter */




/* set up and run a processor */
void
ShufflePlugin::setupAndProcess(ShufflerBase &processor, const OFX::RenderArguments &args)
{
    std::auto_ptr<OFX::Image> dst(_dstClip->fetchImage(args.time));
    if (!dst.get()) {
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    const double time = args.time;
    OFX::BitDepthEnum         dstBitDepth    = dst->getPixelDepth();
    OFX::PixelComponentEnum   dstComponents  = dst->getPixelComponents();
    if (dstBitDepth != _dstClip->getPixelDepth() ||
        dstComponents != _dstClip->getPixelComponents()) {
        setPersistentMessage(OFX::Message::eMessageError, "", "OFX Host gave image with wrong depth or components");
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    if (dst->getRenderScale().x != args.renderScale.x ||
        dst->getRenderScale().y != args.renderScale.y ||
        (dst->getField() != OFX::eFieldNone /* for DaVinci Resolve */ && dst->getField() != args.fieldToRender)) {
        setPersistentMessage(OFX::Message::eMessageError, "", "OFX Host gave image with wrong scale or field properties");
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    
    
    InputChannelEnum r,g,b,a;
    // compute the components mapping tables
    std::vector<InputChannelEnum> channelMap;
    
    std::auto_ptr<const OFX::Image> srcA((_srcClipA && _srcClipA->isConnected()) ?
                                         _srcClipA->fetchImage(args.time) : 0);
    std::auto_ptr<const OFX::Image> srcB((_srcClipB && _srcClipB->isConnected()) ?
                                         _srcClipB->fetchImage(args.time) : 0);
    OFX::BitDepthEnum srcBitDepth = eBitDepthNone;
    OFX::PixelComponentEnum srcComponents = ePixelComponentNone;
    if (srcA.get()) {
        if (srcA->getRenderScale().x != args.renderScale.x ||
            srcA->getRenderScale().y != args.renderScale.y ||
            (srcA->getField() != OFX::eFieldNone /* for DaVinci Resolve */ && srcA->getField() != args.fieldToRender)) {
            setPersistentMessage(OFX::Message::eMessageError, "", "OFX Host gave image with wrong scale or field properties");
            OFX::throwSuiteStatusException(kOfxStatFailed);
        }
        srcBitDepth      = srcA->getPixelDepth();
        srcComponents = srcA->getPixelComponents();
        assert(_srcClipA->getPixelComponents() == srcComponents);
    }
    
    if (srcB.get()) {
        if (srcB->getRenderScale().x != args.renderScale.x ||
            srcB->getRenderScale().y != args.renderScale.y ||
            (srcB->getField() != OFX::eFieldNone /* for DaVinci Resolve */ && srcB->getField() != args.fieldToRender)) {
            setPersistentMessage(OFX::Message::eMessageError, "", "OFX Host gave image with wrong scale or field properties");
            OFX::throwSuiteStatusException(kOfxStatFailed);
        }
        OFX::BitDepthEnum    srcBBitDepth      = srcB->getPixelDepth();
        OFX::PixelComponentEnum srcBComponents = srcB->getPixelComponents();
        assert(_srcClipB->getPixelComponents() == srcBComponents);
        // both input must have the same bit depth and components
        if ((srcBitDepth != eBitDepthNone && srcBitDepth != srcBBitDepth) ||
            (srcComponents != ePixelComponentNone && srcComponents != srcBComponents)) {
            OFX::throwSuiteStatusException(kOfxStatErrImageFormat);
        }
    }
    
    r = (InputChannelEnum)_channelParam[0]->getValueAtTime(time);
    g = (InputChannelEnum)_channelParam[1]->getValueAtTime(time);
    b = (InputChannelEnum)_channelParam[2]->getValueAtTime(time);
    a = (InputChannelEnum)_channelParam[3]->getValueAtTime(time);
    
    
    switch (dstComponents) {
        case OFX::ePixelComponentRGBA:
            channelMap.resize(4);
            channelMap[0] = r;
            channelMap[1] = g;
            channelMap[2] = b;
            channelMap[3] = a;
            break;
        case OFX::ePixelComponentXY:
            channelMap.resize(2);
            channelMap[0] = r;
            channelMap[1] = g;
            break;
        case OFX::ePixelComponentRGB:
            channelMap.resize(3);
            channelMap[0] = r;
            channelMap[1] = g;
            channelMap[2] = b;
            break;
        case OFX::ePixelComponentAlpha:
            channelMap.resize(1);
            channelMap[0] = a;
            break;
        default:
            channelMap.resize(0);
            break;
    }
    processor.setSrcImg(srcA.get(),srcB.get());
    
    PixelComponentEnum outputComponents = gOutputComponentsMap[_outputLayer->getValueAtTime(time)];
    assert(dstComponents == outputComponents);
    BitDepthEnum outputBitDepth = srcBitDepth;
    if (getImageEffectHostDescription()->supportsMultipleClipDepths) {
        outputBitDepth = gOutputBitDepthMap[_outputBitDepth->getValueAtTime(time)];
    }
    assert(outputBitDepth == dstBitDepth);
    int outputComponentCount = dst->getPixelComponentCount();

    processor.setValues(outputComponents, outputComponentCount, outputBitDepth, channelMap);
    
    processor.setDstImg(dst.get());
    processor.setRenderWindow(args.renderWindow);

    processor.process();
}

class InputImagesHolder_RAII
{
    std::vector<OFX::Image*> images;
public:
    
    InputImagesHolder_RAII()
    : images()
    {
        
    }
    
    void appendImage(OFX::Image* img)
    {
        images.push_back(img);
    }
    
    ~InputImagesHolder_RAII()
    {
        for (std::size_t i = 0; i < images.size(); ++i) {
            delete images[i];
        }
    }
};

void
ShufflePlugin::setupAndProcessMultiPlane(MultiPlaneShufflerBase & processor, const OFX::RenderArguments &args)
{
    const double time = args.time;
    std::string dstOfxPlane,dstOfxComp;
    std::list<std::string> outputComponents = _dstClip->getComponentsPresent();
    OFX::MultiPlane::getPlaneNeededInOutput(outputComponents, _dstClip, _outputLayer, &dstOfxPlane, &dstOfxComp);
    
#ifdef DEBUG
    // Follow the OpenFX spec:
    // check that dstComponents is consistent with the result of getClipPreferences
    // (@see getClipPreferences).
    OFX::PixelComponentEnum pixelComps = mapStrToPixelComponentEnum(dstOfxComp);
    OFX::PixelComponentEnum dstClipComps = _dstClip->getPixelComponents();
    if (pixelComps != OFX::ePixelComponentCustom) {
        assert(dstClipComps == pixelComps);
    } else {
        int nComps = std::max((int)mapPixelComponentCustomToLayerChannels(dstOfxComp).size() - 1, 0);
        switch (nComps) {
            case 1:
                pixelComps = OFX::ePixelComponentAlpha;
                break;
            case 2:
                pixelComps = OFX::ePixelComponentXY;
                break;
            case 3:
                pixelComps = OFX::ePixelComponentRGB;
                break;
            case 4:
                pixelComps = OFX::ePixelComponentRGBA;
            default:
                break;
        }
        assert(dstClipComps == pixelComps);
    }
#endif
    
    std::auto_ptr<OFX::Image> dst(_dstClip->fetchImagePlane(args.time, args.renderView, dstOfxPlane.c_str()));
    if (!dst.get()) {
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    OFX::BitDepthEnum         dstBitDepth    = dst->getPixelDepth();
    int nDstComponents = dst->getPixelComponentCount();
    if (dstBitDepth != _dstClip->getPixelDepth() ||
        nDstComponents != _dstClip->getPixelComponentCount()) {
        setPersistentMessage(OFX::Message::eMessageError, "", "OFX Host gave image with wrong depth or components");
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    if (dst->getRenderScale().x != args.renderScale.x ||
        dst->getRenderScale().y != args.renderScale.y ||
        (dst->getField() != OFX::eFieldNone /* for DaVinci Resolve */ && dst->getField() != args.fieldToRender)) {
        setPersistentMessage(OFX::Message::eMessageError, "", "OFX Host gave image with wrong scale or field properties");
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    
    

    OFX::MultiPlane::PerClipComponentsMap perClipComps;
    OFX::MultiPlane::ClipsComponentsInfoBase& aInfo = perClipComps[kClipA];
    aInfo.clip = _srcClipA;
    aInfo.componentsPresent = _srcClipA->getComponentsPresent();
    
    OFX::MultiPlane::ClipsComponentsInfoBase& bInfo = perClipComps[kClipB];
    bInfo.clip = _srcClipB;
    bInfo.componentsPresent = _srcClipB->getComponentsPresent();
    
    
    InputImagesHolder_RAII imagesHolder;
    OFX::BitDepthEnum srcBitDepth = eBitDepthNone;
    
    std::map<OFX::Clip*,std::map<std::string,OFX::Image*> > fetchedPlanes;
    
    std::vector<InputPlaneChannel> planes;
    bool isCreatingAlpha;
    for (int i = 0; i < nDstComponents; ++i) {
        
        InputPlaneChannel p;
        OFX::Clip* clip = 0;
        std::string plane,ofxComp;
        bool ok = OFX::MultiPlane::getPlaneNeededForParam(time, perClipComps, nDstComponents == 1 ? _channelParam[3] : _channelParam[i], &clip, &plane, &ofxComp, &p.channelIndex, &isCreatingAlpha);
        if (!ok) {
            setPersistentMessage(OFX::Message::eMessageError, "", "Cannot find requested channels in input");
            OFX::throwSuiteStatusException(kOfxStatFailed);
        }
        
        p.img = 0;
        if (ofxComp == kMultiPlaneParamOutputOption0) {
            p.fillZero = true;
        } else if (ofxComp == kMultiPlaneParamOutputOption1) {
            p.fillZero = false;
        } else {
            std::map<std::string,OFX::Image*>& clipPlanes = fetchedPlanes[clip];
            std::map<std::string,OFX::Image*>::iterator foundPlane = clipPlanes.find(plane);
            if (foundPlane != clipPlanes.end()) {
                p.img = foundPlane->second;
            } else {
                p.img = clip->fetchImagePlane(args.time, args.renderView, plane.c_str());
                if (p.img) {
                    clipPlanes.insert(std::make_pair(plane, p.img));
                    imagesHolder.appendImage(p.img);
                }

            }
        }
        
        if (p.img) {
            
            if (p.img->getRenderScale().x != args.renderScale.x ||
                p.img->getRenderScale().y != args.renderScale.y ||
                (p.img->getField() != OFX::eFieldNone /* for DaVinci Resolve */ && p.img->getField() != args.fieldToRender)) {
                setPersistentMessage(OFX::Message::eMessageError, "", "OFX Host gave image with wrong scale or field properties");
                OFX::throwSuiteStatusException(kOfxStatFailed);
            }
            if (srcBitDepth == eBitDepthNone) {
                srcBitDepth = p.img->getPixelDepth();
            } else {
                // both input must have the same bit depth and components
                if (srcBitDepth != eBitDepthNone && srcBitDepth != p.img->getPixelDepth()) {
                    OFX::throwSuiteStatusException(kOfxStatErrImageFormat);
                }
            }
        }
        planes.push_back(p);
    }
    
    BitDepthEnum outputBitDepth = srcBitDepth;
    if (getImageEffectHostDescription()->supportsMultipleClipDepths) {
        outputBitDepth = gOutputBitDepthMap[_outputBitDepth->getValueAtTime(time)];
    }
    assert(outputBitDepth == dstBitDepth);

    processor.setValues(nDstComponents, outputBitDepth, planes);

    processor.setDstImg(dst.get());
    processor.setRenderWindow(args.renderWindow);
    
    processor.process();
}

template <class DSTPIX, int nComponentsDst>
void
ShufflePlugin::renderInternalForDstBitDepth(const OFX::RenderArguments &args, OFX::BitDepthEnum srcBitDepth)
{
    if (!gIsMultiPlanar || !gSupportsDynamicChoices) {
        switch (srcBitDepth) {
            case OFX::eBitDepthUByte : {
                Shuffler<unsigned char, DSTPIX, nComponentsDst> fred(*this);
                setupAndProcess(fred, args);
            }
                break;
            case OFX::eBitDepthUShort : {
                Shuffler<unsigned short, DSTPIX, nComponentsDst> fred(*this);
                setupAndProcess(fred, args);
            }
                break;
            case OFX::eBitDepthFloat : {
                Shuffler<float, DSTPIX, nComponentsDst> fred(*this);
                setupAndProcess(fred, args);
            }
                break;
            default :
                OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
        }
    } else {
        switch (srcBitDepth) {
            case OFX::eBitDepthUByte : {
                MultiPlaneShuffler<unsigned char, DSTPIX, nComponentsDst> fred(*this);
                setupAndProcessMultiPlane(fred, args);
            }
                break;
            case OFX::eBitDepthUShort : {
                MultiPlaneShuffler<unsigned short, DSTPIX, nComponentsDst> fred(*this);
                setupAndProcessMultiPlane(fred, args);
            }
                break;
            case OFX::eBitDepthFloat : {
                MultiPlaneShuffler<float, DSTPIX, nComponentsDst> fred(*this);
                setupAndProcessMultiPlane(fred, args);
            }
                break;
            default :
                OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
        }

    }
}

// the internal render function
template <int nComponentsDst>
void
ShufflePlugin::renderInternal(const OFX::RenderArguments &args, OFX::BitDepthEnum srcBitDepth, OFX::BitDepthEnum dstBitDepth)
{
    switch (dstBitDepth) {
        case OFX::eBitDepthUByte :
            renderInternalForDstBitDepth<unsigned char, nComponentsDst>(args, srcBitDepth);
            break;
        case OFX::eBitDepthUShort :
            renderInternalForDstBitDepth<unsigned short, nComponentsDst>(args, srcBitDepth);
            break;
        case OFX::eBitDepthFloat :
            renderInternalForDstBitDepth<float, nComponentsDst>(args, srcBitDepth);
            break;
        default :
            OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
}

// the overridden render function
void
ShufflePlugin::render(const OFX::RenderArguments &args)
{
    assert (_srcClipA && _srcClipB && _dstClip);
    if (!_srcClipA || !_srcClipB || !_dstClip) {
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    const double time = args.time;
    // instantiate the render code based on the pixel depth of the dst clip
    OFX::BitDepthEnum       dstBitDepth    = _dstClip->getPixelDepth();
    OFX::PixelComponentEnum dstComponents  = _dstClip->getPixelComponents();
#ifdef DEBUG
    // Follow the OpenFX spec:
    // check that dstComponents is consistent with the result of getClipPreferences
    // (@see getClipPreferences).
    if (gIsMultiPlanar && gSupportsDynamicChoices) {
        std::list<std::string> outputComponents = _dstClip->getComponentsPresent();
        std::string ofxPlane,ofxComponents;
        OFX::MultiPlane::getPlaneNeededInOutput(outputComponents, _dstClip, _outputLayer, &ofxPlane, &ofxComponents);

        OFX::PixelComponentEnum pixelComps = mapStrToPixelComponentEnum(ofxComponents);
        if (pixelComps == OFX::ePixelComponentCustom) {
            int nComps = std::max((int)mapPixelComponentCustomToLayerChannels(ofxComponents).size() - 1, 0);
            switch (nComps) {
                case 1:
                    pixelComps = OFX::ePixelComponentAlpha;
                    break;
                case 2:
                    pixelComps = OFX::ePixelComponentXY;
                    break;
                case 3:
                    pixelComps = OFX::ePixelComponentRGB;
                    break;
                case 4:
                    pixelComps = OFX::ePixelComponentRGBA;
                default:
                    break;
            }
        }
        assert(dstComponents == pixelComps);
    } else {
        // set the components of _dstClip
        PixelComponentEnum outputComponents = gOutputComponentsMap[_outputLayer->getValueAtTime(time)];
        assert(dstComponents == outputComponents);
    }

    if (getImageEffectHostDescription()->supportsMultipleClipDepths) {
        // set the bitDepth of _dstClip
        BitDepthEnum outputBitDepth = gOutputBitDepthMap[_outputBitDepth->getValueAtTime(time)];
        assert(dstBitDepth == outputBitDepth);
    }
#endif
    int dstComponentCount  = _dstClip->getPixelComponentCount();
    assert(1 <= dstComponentCount && dstComponentCount <= 4);

    assert(kSupportsMultipleClipPARs   || _srcClipA->getPixelAspectRatio() == _dstClip->getPixelAspectRatio());
    assert(kSupportsMultipleClipDepths || _srcClipA->getPixelDepth()       == _dstClip->getPixelDepth());
    assert(kSupportsMultipleClipPARs   || _srcClipB->getPixelAspectRatio() == _dstClip->getPixelAspectRatio());
    assert(kSupportsMultipleClipDepths || _srcClipB->getPixelDepth()       == _dstClip->getPixelDepth());
    // get the components of _dstClip
    
    if (!gIsMultiPlanar) {
        PixelComponentEnum outputComponents = gOutputComponentsMap[_outputLayer->getValueAtTime(time)];
        if (dstComponents != outputComponents) {
            setPersistentMessage(OFX::Message::eMessageError, "", "Shuffle: OFX Host did not take into account output components");
            OFX::throwSuiteStatusException(kOfxStatErrImageFormat);
        }
    }
    
    if (getImageEffectHostDescription()->supportsMultipleClipDepths) {
        // get the bitDepth of _dstClip
        BitDepthEnum outputBitDepth = gOutputBitDepthMap[_outputBitDepth->getValueAtTime(time)];
        if (dstBitDepth != outputBitDepth) {
            setPersistentMessage(OFX::Message::eMessageError, "", "Shuffle: OFX Host did not take into account output bit depth");
            OFX::throwSuiteStatusException(kOfxStatErrImageFormat);
        }
    }

    OFX::BitDepthEnum srcBitDepth = _srcClipA->getPixelDepth();

    if (_srcClipA->isConnected() && _srcClipB->isConnected()) {
        OFX::BitDepthEnum srcBBitDepth = _srcClipB->getPixelDepth();
        // both input must have the same bit depth
        if (srcBitDepth != srcBBitDepth) {
            setPersistentMessage(OFX::Message::eMessageError, "", "Shuffle: both inputs must have the same bit depth");
            OFX::throwSuiteStatusException(kOfxStatErrImageFormat);
        }
    }

    switch (dstComponentCount) {
        case 4:
            renderInternal<4>(args, srcBitDepth, dstBitDepth);
            break;
        case 3:
            renderInternal<3>(args, srcBitDepth, dstBitDepth);
            break;
        case 2:
            renderInternal<2>(args, srcBitDepth, dstBitDepth);
            break;
        case 1:
            renderInternal<1>(args, srcBitDepth, dstBitDepth);
            break;
    }
}


/* Override the clip preferences */
void
ShufflePlugin::getClipPreferences(OFX::ClipPreferencesSetter &clipPreferences)
{
    PixelComponentEnum originalDstPixelComps = OFX::ePixelComponentNone;
    PixelComponentEnum dstPixelComps = OFX::ePixelComponentNone;
    if (gIsMultiPlanar && gSupportsDynamicChoices) {
    
        std::vector<OFX::MultiPlane::ClipComponentsInfo> clipInfos(2);
        OFX::MultiPlane::ClipComponentsInfo& aInfo = clipInfos[0];
        OFX::MultiPlane::ClipComponentsInfo& bInfo = clipInfos[1];
        aInfo.clip = _srcClipA;
        bInfo.clip = _srcClipB;
        aInfo.componentsPresent = _srcClipA->getComponentsPresent();
        bInfo.componentsPresent = _srcClipB->getComponentsPresent();
        aInfo.componentsPresentCache = &_currentCompsA;
        bInfo.componentsPresentCache = &_currentCompsB;
        
        std::vector<OFX::MultiPlane::ChoiceParamClips> choiceParams(5);
        for (int i = 0; i < 4; ++i) {
            choiceParams[i].param = _channelParam[i];
            choiceParams[i].stringparam = _channelParamStrings[i];
            choiceParams[i].clips = &clipInfos;
        }
        
        std::vector<OFX::MultiPlane::ClipComponentsInfo> outputClipInfos(1);
        outputClipInfos[0].clip = _dstClip;
        outputClipInfos[0].componentsPresent = _dstClip->getComponentsPresent();
        outputClipInfos[0].componentsPresentCache = &_currentOutputComps;
        choiceParams[4].param = _outputLayer;
        choiceParams[4].stringparam = _outputLayerString;
        choiceParams[4].clips = &outputClipInfos;
        
        //choiceParams[4].clips = _outputLayer;
        
        OFX::MultiPlane::buildChannelMenus(choiceParams);
        std::string ofxPlane,ofxComponents;
        OFX::MultiPlane::getPlaneNeededInOutput(outputClipInfos[0].componentsPresent, _dstClip, _outputLayer, &ofxPlane, &ofxComponents);
        
        dstPixelComps = mapStrToPixelComponentEnum(ofxComponents);
        originalDstPixelComps = dstPixelComps;
        if (dstPixelComps == OFX::ePixelComponentCustom) {
            int nComps = std::max((int)mapPixelComponentCustomToLayerChannels(ofxComponents).size() - 1, 0);
            switch (nComps) {
                case 1:
                    dstPixelComps = OFX::ePixelComponentAlpha;
                    break;
                case 2:
                    dstPixelComps = OFX::ePixelComponentXY;
                    break;
                case 3:
                    dstPixelComps = OFX::ePixelComponentRGB;
                    break;
                case 4:
                    dstPixelComps = OFX::ePixelComponentRGBA;
                default:
                    break;
            }
        } else if (dstPixelComps == OFX::ePixelComponentAlpha ||
                   dstPixelComps == OFX::ePixelComponentRGB ||
                   dstPixelComps == OFX::ePixelComponentRGBA) {
            //If color plane, select the value chosen by the user from the output components choice
            int comps_i;
            _outputComponents->getValue(comps_i);
            dstPixelComps = gOutputComponentsMap[comps_i];
        }
    } else {
        // set the components of _dstClip
        dstPixelComps = gOutputComponentsMap[_outputLayer->getValue()];
        originalDstPixelComps = dstPixelComps;
    }
    
    clipPreferences.setClipComponents(*_dstClip, dstPixelComps);

    
    //Enable components according to the new dstPixelComps
    enableComponents(originalDstPixelComps,dstPixelComps);

    if (getImageEffectHostDescription()->supportsMultipleClipDepths) {
        // set the bitDepth of _dstClip
        BitDepthEnum outputBitDepth = gOutputBitDepthMap[_outputBitDepth->getValue()];
        clipPreferences.setClipBitDepth(*_dstClip, outputBitDepth);
    }
    
    OFX::PreMultiplicationEnum premult;
    int premult_i;
    _outputPremult->getValue(premult_i);
    switch (premult_i) {
        case 0:
            premult = OFX::eImageOpaque;
            break;
        case 1:
            premult = OFX::eImagePreMultiplied;
            break;
        case 2:
            premult = OFX::eImageUnPreMultiplied;
            break;
        default:
            assert(false);
            break;
    }
    clipPreferences.setOutputPremultiplication(premult);

}

static std::string
imageFormatString(PixelComponentEnum components, BitDepthEnum bitDepth)
{
    std::string s;
    switch (components) {
        case OFX::ePixelComponentRGBA:
            s += "RGBA";
            break;
        case OFX::ePixelComponentRGB:
            s += "RGB";
            break;
        case OFX::ePixelComponentAlpha:
            s += "Alpha";
            break;
#ifdef OFX_EXTENSIONS_NUKE
        case OFX::ePixelComponentMotionVectors:
            s += "MotionVectors";
            break;
        case OFX::ePixelComponentStereoDisparity:
            s += "StereoDisparity";
            break;
#endif
#ifdef OFX_EXTENSIONS_NATRON
        case OFX::ePixelComponentXY:
            s += "XY";
            break;
#endif
        case OFX::ePixelComponentCustom:
            s += "Custom";
            break;
        case OFX::ePixelComponentNone:
            s += "None";
            break;
        default:
            s += "[unknown components]";
            break;
    }
    switch (bitDepth) {
        case OFX::eBitDepthUByte:
            s += "8u";
            break;
        case OFX::eBitDepthUShort:
            s += "16u";
            break;
        case OFX::eBitDepthFloat:
            s += "32f";
            break;
        case OFX::eBitDepthCustom:
            s += "x";
            break;
        case OFX::eBitDepthNone:
            s += "0";
            break;
#ifdef OFX_EXTENSIONS_VEGAS
        case OFX::eBitDepthUByteBGRA:
            s += "8uBGRA";
            break;
        case OFX::eBitDepthUShortBGRA:
            s += "16uBGRA";
            break;
        case OFX::eBitDepthFloatBGRA:
            s += "32fBGRA";
            break;
#endif
        default:
            s += "[unknown bit depth]";
            break;
    }
    return s;
}

static bool endsWith(const std::string &str, const std::string &suffix)
{
    return ((str.size() >= suffix.size()) &&
            (str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0));
}

void
ShufflePlugin::setChannelsFromRed(double time)
{
    int r_i;
    _channelParam[0]->getValueAtTime(time, r_i);
    std::string rChannel;
    _channelParam[0]->getOption(r_i, rChannel);
    
    if (endsWith(rChannel, ".R") || endsWith(rChannel, ".r")) {
        std::string base = rChannel.substr(0,rChannel.size() - 2);
        
        bool gSet = false;
        bool bSet = false;
        bool aSet = false;
        
        int nOpt = _channelParam[1]->getNOptions();
        
        int indexOf0 = -1;
        int indexOf1 = -1;
        
        for (int i = 0; i < nOpt; ++i) {
            std::string opt;
            _channelParam[0]->getOption(i, opt);
            
            if (opt == kMultiPlaneParamOutputOption0) {
                indexOf0 = i;
            } else if (opt == kMultiPlaneParamOutputOption1) {
                indexOf1 = i;
            } else if (opt.substr(0,base.size()) == base) {
                std::string chan = opt.substr(base.size());
                if (chan == ".G" || chan == ".g") {
                    _channelParam[1]->setValue(i);
                    if (_channelParamStrings[1]) {
                        _channelParamStrings[1]->setValue(opt);
                    }
                    gSet = true;
                } else if (chan == ".B" || chan == ".b") {
                    _channelParam[2]->setValue(i);
                    if (_channelParamStrings[2]) {
                        _channelParamStrings[2]->setValue(opt);
                    }
                    bSet = true;
                } else if (chan == ".A" || chan == ".a") {
                    _channelParam[3]->setValue(i);
                    if (_channelParamStrings[3]) {
                        _channelParamStrings[3]->setValue(opt);
                    }
                    aSet = true;
                }
            }
            if (gSet && bSet && aSet && indexOf0 != -1 && indexOf1 != -1) {
                // we're done
                break;
            }
        }
        assert(indexOf0 != -1 && indexOf1 != -1);
        if (!gSet) {
            _channelParam[1]->setValue(indexOf0);
            if (_channelParamStrings[1]) {
                _channelParamStrings[1]->setValue(kMultiPlaneParamOutputOption0);
            }
        }
        if (!bSet) {
            _channelParam[2]->setValue(indexOf0);
            if (_channelParamStrings[2]) {
                _channelParamStrings[2]->setValue(kMultiPlaneParamOutputOption0);
            }
        }
        if (!aSet) {
            _channelParam[3]->setValue(indexOf1);
            if (_channelParamStrings[3]) {
                _channelParamStrings[3]->setValue(kMultiPlaneParamOutputOption0);
            }
        }
    }
}

void
ShufflePlugin::setOutputComponentsParam(OFX::PixelComponentEnum components)
{
    assert(components == OFX::ePixelComponentRGB || components == OFX::ePixelComponentRGBA || components == OFX::ePixelComponentAlpha);
    int index = -1;
    for (int i = 0; i < 4; ++i) {
        if (gOutputComponentsMap[i] == OFX::ePixelComponentNone) {
            break;
        }
        if (gOutputComponentsMap[i] == components) {
            index = i;
            break;
        }
    }
    assert(index != -1);
    _outputComponents->setValue(index);
}

void
ShufflePlugin::changedParam(const OFX::InstanceChangedArgs &args, const std::string &paramName)
{
    //Commented out as it cannot be done here: enableComponents() relies on the clip components but the clip
    //components might not yet be set if the user changed a clip pref slaved param. Instead we have to move it into the getClipPreferences
    /*if (paramName == kParamOutputComponents || paramName == kParamOutputChannels) {
        enableComponents();
    } else*/
    if (paramName == kParamClipInfo && args.reason == eChangeUserEdit) {
        std::string msg;
        msg += "Input A: ";
        if (!_srcClipA) {
            msg += "N/A";
        } else {
            msg += imageFormatString(_srcClipA->getPixelComponents(), _srcClipA->getPixelDepth());
        }
        msg += "\n";
        if (getContext() == eContextGeneral) {
            msg += "Input B: ";
            if (!_srcClipB) {
                msg += "N/A";
            } else {
                msg += imageFormatString(_srcClipB->getPixelComponents(), _srcClipB->getPixelDepth());
            }
            msg += "\n";
        }
        msg += "Output: ";
        if (!_dstClip) {
            msg += "N/A";
        } else {
            msg += imageFormatString(_dstClip->getPixelComponents(), _dstClip->getPixelDepth());
        }
        msg += "\n";
        sendMessage(OFX::Message::eMessageMessage, "", msg);
    } else if (paramName == kParamOutputChannelsChoice) {
        
        int layer_i;
        _outputLayer->getValue(layer_i);
        std::string layerName;
        _outputLayer->getOption(layer_i, layerName);
        
        std::string ofxComponents;
        if (layerName.empty() ||
            layerName == kPlaneLabelColorRGBA ||
            layerName == kPlaneLabelColorRGB ||
            layerName == kPlaneLabelColorAlpha) {
            ofxComponents = _dstClip->getPixelComponentsProperty();
        }
        bool secret = false;
        if (ofxComponents == kOfxImageComponentAlpha) {
            setOutputComponentsParam(OFX::ePixelComponentAlpha);
        } else if (ofxComponents == kOfxImageComponentRGB) {
            setOutputComponentsParam(OFX::ePixelComponentRGB);
        } else if (ofxComponents == kOfxImageComponentRGBA) {
            setOutputComponentsParam(OFX::ePixelComponentRGBA);
        } else {
            secret = true;
        }
        _outputComponents->setIsSecret(secret);

    }
    
    
    if (gIsMultiPlanar && gSupportsDynamicChoices) {
        OFX::MultiPlane::ChangedParamRetCode trappedRParam =
        OFX::MultiPlane::checkIfChangedParamCalledOnDynamicChoice(paramName, args.reason, _channelParam[0], _channelParamStrings[0]);
        if (trappedRParam != OFX::MultiPlane::eChangedParamRetCodeNoChange) {
            if (trappedRParam == OFX::MultiPlane::eChangedParamRetCodeChoiceParamChanged) {
#ifdef OFX_EXTENSIONS_NATRON
                setChannelsFromRed(args.time);
#endif
            }
            return;
        }
        for (int i = 1; i < 4; ++i) {
            if (OFX::MultiPlane::checkIfChangedParamCalledOnDynamicChoice(paramName, args.reason, _channelParam[i], _channelParamStrings[i])) {
                return;
            }
        }
        
        if (OFX::MultiPlane::checkIfChangedParamCalledOnDynamicChoice(paramName, args.reason, _outputLayer, _outputLayerString)) {
            return;
        }
    }
}

void
ShufflePlugin::changedClip(const InstanceChangedArgs &/*args*/, const std::string &clipName)
{
    if (getContext() == eContextGeneral &&
        (clipName == kClipA || clipName == kClipB)) {
        // check that A and B are compatible if they're both connected
        if (_srcClipA && _srcClipA->isConnected() && _srcClipB && _srcClipB->isConnected()) {
            OFX::BitDepthEnum srcABitDepth = _srcClipA->getPixelDepth();
            OFX::BitDepthEnum srcBBitDepth = _srcClipB->getPixelDepth();
            // both input must have the same bit depth
            if (srcABitDepth != srcBBitDepth) {
                setPersistentMessage(OFX::Message::eMessageError, "", "Shuffle: both inputs must have the same bit depth");
                OFX::throwSuiteStatusException(kOfxStatErrImageFormat);
            }
        }
    }
}


void
ShufflePlugin::enableComponents(PixelComponentEnum originalOutputComponents, PixelComponentEnum outputComponentsWithCreateAlpha)
{
    if (!gIsMultiPlanar) {
        int outputComponents_i;
        _outputLayer->getValue(outputComponents_i);
        switch (gOutputComponentsMap[outputComponents_i]) {
            case ePixelComponentRGBA:
                for (int i = 0; i < 4; ++i) {
                  _channelParam[i]->setEnabled(true);
                }
                break;
            case ePixelComponentRGB:
                _channelParam[0]->setEnabled(true);
                _channelParam[1]->setEnabled(true);
                _channelParam[2]->setEnabled(true);
                _channelParam[3]->setEnabled(false);
                break;
            case ePixelComponentAlpha:
                _channelParam[0]->setEnabled(false);
                _channelParam[1]->setEnabled(false);
                _channelParam[2]->setEnabled(false);
                _channelParam[3]->setEnabled(true);
                break;
#ifdef OFX_EXTENSIONS_NUKE
            case ePixelComponentMotionVectors:
            case ePixelComponentStereoDisparity:
                _channelParam[0]->setEnabled(true);
                _channelParam[1]->setEnabled(true);
                _channelParam[2]->setEnabled(false);
                _channelParam[3]->setEnabled(false);
                break;
#endif
#ifdef OFX_EXTENSIONS_NATRON
            case ePixelComponentXY:
                _channelParam[0]->setEnabled(true);
                _channelParam[1]->setEnabled(true);
                _channelParam[2]->setEnabled(false);
                _channelParam[3]->setEnabled(false);
                break;
#endif
            default:
                assert(0);
                break;
        }
    } else { // if (!gIsMultiPlanar) {
        std::list<std::string> components =  _dstClip->getComponentsPresent();
        
        std::string ofxPlane,ofxComp;
        OFX::MultiPlane::getPlaneNeededInOutput(components, _dstClip, _outputLayer, &ofxPlane, &ofxComp);
        std::vector<std::string> compNames;
        
        
        bool showCreateAlpha = false;
        if (ofxPlane == kFnOfxImagePlaneColour) {
            //std::string comp = _dstClip->getPixelComponentsProperty();
            if (outputComponentsWithCreateAlpha == OFX::ePixelComponentRGB) {
                compNames.push_back("R");
                compNames.push_back("G");
                compNames.push_back("B");
                showCreateAlpha = true;
            } else if (outputComponentsWithCreateAlpha == OFX::ePixelComponentRGBA) {
                compNames.push_back("R");
                compNames.push_back("G");
                compNames.push_back("B");
                compNames.push_back("A");
                
                if (originalOutputComponents != OFX::ePixelComponentRGBA) {
                    showCreateAlpha = true;
                }
            } else if (outputComponentsWithCreateAlpha == OFX::ePixelComponentAlpha) {
                compNames.push_back("Alpha");
            }

        } else if (ofxComp == kFnOfxImageComponentStereoDisparity) {
            compNames.push_back("X");
            compNames.push_back("Y");
        } else if (ofxComp == kFnOfxImageComponentMotionVectors) {
            compNames.push_back("U");
            compNames.push_back("V");
#ifdef OFX_EXTENSIONS_NATRON
        } else {
            std::vector<std::string> layerChannels = mapPixelComponentCustomToLayerChannels(ofxComp);
            if (layerChannels.size() >= 1) {
                compNames.assign(layerChannels.begin() + 1, layerChannels.end());
            }

#endif
        }
        
        
        if (compNames.size() == 1) {
            _channelParam[0]->setEnabled(false);
            _channelParam[0]->setIsSecret(true);
            _channelParam[1]->setEnabled(false);
            _channelParam[1]->setIsSecret(true);
            _channelParam[2]->setEnabled(false);
            _channelParam[2]->setIsSecret(true);
            _channelParam[3]->setEnabled(true);
            _channelParam[3]->setIsSecret(false);
            _channelParam[3]->setLabel(compNames[0]);
        } else if (compNames.size() == 2) {
            _channelParam[0]->setEnabled(true);
            _channelParam[0]->setIsSecret(false);
            _channelParam[0]->setLabel(compNames[0]);
            _channelParam[1]->setEnabled(true);
            _channelParam[1]->setIsSecret(false);
            _channelParam[1]->setLabel(compNames[1]);
            _channelParam[2]->setEnabled(false);
            _channelParam[2]->setIsSecret(true);
            _channelParam[3]->setEnabled(false);
            _channelParam[3]->setIsSecret(true);
        } else if (compNames.size() == 3) {
            _channelParam[0]->setEnabled(true);
            _channelParam[0]->setIsSecret(false);
            _channelParam[0]->setLabel(compNames[0]);
            _channelParam[1]->setEnabled(true);
            _channelParam[1]->setLabel(compNames[1]);
            _channelParam[1]->setIsSecret(false);
            _channelParam[2]->setEnabled(true);
            _channelParam[2]->setIsSecret(false);
            _channelParam[2]->setLabel(compNames[2]);
            _channelParam[3]->setEnabled(false);
            _channelParam[3]->setIsSecret(true);

        } else if (compNames.size() == 4) {
            _channelParam[0]->setEnabled(true);
            _channelParam[0]->setIsSecret(false);
            _channelParam[0]->setLabel(compNames[0]);
            _channelParam[1]->setEnabled(true);
            _channelParam[1]->setLabel(compNames[1]);
            _channelParam[1]->setIsSecret(false);
            _channelParam[2]->setEnabled(true);
            _channelParam[2]->setIsSecret(false);
            _channelParam[2]->setLabel(compNames[2]);
            _channelParam[3]->setEnabled(true);
            _channelParam[3]->setIsSecret(false);
            _channelParam[3]->setLabel(compNames[3]);
        } else {
            //Unsupported
            throwSuiteStatusException(kOfxStatFailed);
        }
    }
}


mDeclarePluginFactory(ShufflePluginFactory, {}, {});

void ShufflePluginFactory::describe(OFX::ImageEffectDescriptor &desc)
{
    // basic labels
    desc.setLabel(kPluginName);
    desc.setPluginGrouping(kPluginGrouping);
    desc.setPluginDescription(kPluginDescription);

    desc.addSupportedContext(eContextFilter);
    desc.addSupportedContext(eContextGeneral);
    desc.addSupportedBitDepth(eBitDepthUByte);
    desc.addSupportedBitDepth(eBitDepthUShort);
    desc.addSupportedBitDepth(eBitDepthFloat);

    if (getImageEffectHostDescription()->supportsMultipleClipDepths) {
        for (ImageEffectHostDescription::PixelDepthArray::const_iterator it = getImageEffectHostDescription()->_supportedPixelDepths.begin();
             it != getImageEffectHostDescription()->_supportedPixelDepths.end();
             ++it) {
            switch (*it) {
                case eBitDepthUByte:
                    gSupportsBytes  = true;
                    break;
                case eBitDepthUShort:
                    gSupportsShorts = true;
                    break;
                case eBitDepthFloat:
                    gSupportsFloats = true;
                    break;
                default:
                    // other bitdepths are not supported by this plugin
                    break;
            }
        }
    }
    {
        int i = 0;
        // Note: gOutputBitDepthMap must have size # of bit depths + 1 !
        if (gSupportsFloats) {
            gOutputBitDepthMap[i] = eBitDepthFloat;
            ++i;
        }
        if (gSupportsShorts) {
            gOutputBitDepthMap[i] = eBitDepthUShort;
            ++i;
        }
        if (gSupportsBytes) {
            gOutputBitDepthMap[i] = eBitDepthUByte;
            ++i;
        }
        assert(sizeof(gOutputBitDepthMap) >= sizeof(gOutputBitDepthMap[0])*(i+1));
        gOutputBitDepthMap[i] = eBitDepthNone;
    }
    for (ImageEffectHostDescription::PixelComponentArray::const_iterator it = getImageEffectHostDescription()->_supportedComponents.begin();
         it != getImageEffectHostDescription()->_supportedComponents.end();
         ++it) {
        switch (*it) {
            case ePixelComponentRGBA:
                gSupportsRGBA  = true;
                break;
            case ePixelComponentRGB:
                gSupportsRGB = true;
                break;
            case ePixelComponentAlpha:
                gSupportsAlpha = true;
                break;
            default:
                // other components are not supported by this plugin
                break;
        }
    }
    {
        int i = 0;
        // Note: gOutputComponentsMap must have size # of component types + 1 !
        if (gSupportsRGBA) {
            gOutputComponentsMap[i] = ePixelComponentRGBA;
            ++i;
        }
        if (gSupportsRGB) {
            gOutputComponentsMap[i] = ePixelComponentRGB;
            ++i;
        }
        if (gSupportsAlpha) {
            gOutputComponentsMap[i] = ePixelComponentAlpha;
            ++i;
        }
        assert(sizeof(gOutputComponentsMap) >= sizeof(gOutputComponentsMap[0])*(i+1));
        gOutputComponentsMap[i] = ePixelComponentNone;
    }

    // set a few flags
    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setSupportsMultiResolution(kSupportsMultiResolution);
    desc.setSupportsTiles(kSupportsTiles);
    desc.setTemporalClipAccess(false);
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipPARs(kSupportsMultipleClipPARs);
    // say we can support multiple pixel depths on in and out
    desc.setSupportsMultipleClipDepths(kSupportsMultipleClipDepths);
    desc.setRenderThreadSafety(kRenderThreadSafety);
    
#ifdef OFX_EXTENSIONS_NATRON
    gSupportsDynamicChoices = OFX::getImageEffectHostDescription()->supportsDynamicChoices;
    
    //Do not add channel selectors, it is pointless
    desc.setChannelSelector(OFX::ePixelComponentNone);
#else
    gSupportsDynamicChoices = false;
#endif
#ifdef OFX_EXTENSIONS_NUKE
    gIsMultiPlanar = kEnableMultiPlanar && OFX::getImageEffectHostDescription()->isMultiPlanar;
    if (gIsMultiPlanar) {
        // This enables fetching different planes from the input.
        // Generally the user will read a multi-layered EXR file in the Reader node and then use the shuffle
        // to redirect the plane's channels into RGBA color plane.

        desc.setIsMultiPlanar(true);
        
        // We are pass-through in output, meaning another shuffle below could very well
        // access all planes again. Note that for multi-planar effects this is mandatory to be called
        // since default is false.
        desc.setPassThroughForNotProcessedPlanes(ePassThroughLevelPassThroughNonRenderedPlanes);
    }
#else 
    gIsMultiPlanar = false;
#endif
}

void ShufflePluginFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum context)
{
    
#ifdef OFX_EXTENSIONS_NUKE
    if (gIsMultiPlanar && !OFX::fetchSuite(kFnOfxImageEffectPlaneSuite, 2)) {
        OFX::throwHostMissingSuiteException(kFnOfxImageEffectPlaneSuite);
    }
#endif
    
    if (context == eContextGeneral) {
        ClipDescriptor* srcClipB = desc.defineClip(kClipB);
        srcClipB->addSupportedComponent(ePixelComponentRGBA);
        srcClipB->addSupportedComponent(ePixelComponentRGB);
        srcClipB->addSupportedComponent(ePixelComponentAlpha);
#ifdef OFX_EXTENSIONS_NATRON
        srcClipB->addSupportedComponent(ePixelComponentXY);
#endif
        srcClipB->setTemporalClipAccess(false);
        srcClipB->setSupportsTiles(kSupportsTiles);
        srcClipB->setOptional(true);

        ClipDescriptor* srcClipA = desc.defineClip(kClipA);
        srcClipA->addSupportedComponent(ePixelComponentRGBA);
        srcClipA->addSupportedComponent(ePixelComponentRGB);
        srcClipA->addSupportedComponent(ePixelComponentAlpha);
#ifdef OFX_EXTENSIONS_NATRON
        srcClipA->addSupportedComponent(ePixelComponentXY);
#endif
        srcClipA->setTemporalClipAccess(false);
        srcClipA->setSupportsTiles(kSupportsTiles);
        srcClipA->setOptional(false);
    } else {
        ClipDescriptor* srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
        srcClip->addSupportedComponent(ePixelComponentRGBA);
        srcClip->addSupportedComponent(ePixelComponentRGB);
        srcClip->addSupportedComponent(ePixelComponentAlpha);
#ifdef OFX_EXTENSIONS_NATRON
        srcClip->addSupportedComponent(ePixelComponentXY);
#endif
        srcClip->setTemporalClipAccess(false);
        srcClip->setSupportsTiles(kSupportsTiles);
    }
    {
        // create the mandated output clip
        ClipDescriptor *dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
        dstClip->addSupportedComponent(ePixelComponentRGBA);
        dstClip->addSupportedComponent(ePixelComponentRGB);
        dstClip->addSupportedComponent(ePixelComponentAlpha);
#ifdef OFX_EXTENSIONS_NATRON
        dstClip->addSupportedComponent(ePixelComponentXY);
#endif
        dstClip->setSupportsTiles(kSupportsTiles);
    }

    // make some pages and to things in
    PageParamDescriptor *page = desc.definePageParam("Controls");

    // outputComponents
    if (gIsMultiPlanar && gSupportsDynamicChoices) {
        OFX::MultiPlane::describeInContextAddOutputLayerChoice(false, desc, page);
    }
    {
        ChoiceParamDescriptor *param = desc.defineChoiceParam(kParamOutputComponents);
        param->setLabel(kParamOutputComponentsLabel);
        param->setHint(kParamOutputComponentsHint);
        // the following must be in the same order as in describe(), so that the map works
        if (gSupportsRGBA) {
            assert(gOutputComponentsMap[param->getNOptions()] == ePixelComponentRGBA);
            param->appendOption(kParamOutputComponentsOptionRGBA);
        }
        if (gSupportsRGB) {
            assert(gOutputComponentsMap[param->getNOptions()] == ePixelComponentRGB);
            param->appendOption(kParamOutputComponentsOptionRGB);
        }
        if (gSupportsAlpha) {
            assert(gOutputComponentsMap[param->getNOptions()] == ePixelComponentAlpha);
            param->appendOption(kParamOutputComponentsOptionAlpha);
        }
        param->setDefault(0);
        param->setAnimates(false);
        desc.addClipPreferencesSlaveParam(*param);
        if (page) {
            page->addChild(*param);
        }
    }
    // ouputBitDepth
    if (getImageEffectHostDescription()->supportsMultipleClipDepths) {
        ChoiceParamDescriptor *param = desc.defineChoiceParam(kParamOutputBitDepth);
        param->setLabel(kParamOutputBitDepthLabel);
        param->setHint(kParamOutputBitDepthHint);
        // the following must be in the same order as in describe(), so that the map works
        if (gSupportsFloats) {
            // coverity[check_return]
            assert(0 <= param->getNOptions() && param->getNOptions() < 4 && gOutputBitDepthMap[param->getNOptions()] == eBitDepthFloat);
            param->appendOption(kParamOutputBitDepthOptionFloat);
        }
        if (gSupportsShorts) {
            // coverity[check_return]
            assert(0 <= param->getNOptions() && param->getNOptions() < 4 && gOutputBitDepthMap[param->getNOptions()] == eBitDepthUShort);
            param->appendOption(kParamOutputBitDepthOptionShort);
        }
        if (gSupportsBytes) {
            // coverity[check_return]
            assert(0 <= param->getNOptions() && param->getNOptions() < 4 && gOutputBitDepthMap[param->getNOptions()] == eBitDepthUByte);
            param->appendOption(kParamOutputBitDepthOptionByte);
        }
        param->setDefault(0);
#ifndef DEBUG
        // Shuffle only does linear conversion, which is useless for 8-bits and 16-bits formats.
        // Disable it for now (in the future, there may be colorspace conversion options)
        param->setIsSecret(true); // always secret
#endif
        param->setAnimates(false);
        desc.addClipPreferencesSlaveParam(*param);
        if (page) {
            page->addChild(*param);
        }
    }
    
    std::vector<std::string> clipsForChannels(2);
    clipsForChannels[0] = kClipA;
    clipsForChannels[1] = kClipB;
    
    if (gSupportsRGB || gSupportsRGBA) {
        // outputR
        if (gIsMultiPlanar && gSupportsDynamicChoices) {
            OFX::ChoiceParamDescriptor* r = OFX::MultiPlane::describeInContextAddChannelChoice(desc, page, clipsForChannels, kParamOutputR, kParamOutputRLabel, kParamOutputRHint);
            r->setDefault(eInputChannelAR);
            OFX::ChoiceParamDescriptor* g = OFX::MultiPlane::describeInContextAddChannelChoice(desc, page, clipsForChannels, kParamOutputG, kParamOutputGLabel, kParamOutputGHint);
            g->setDefault(eInputChannelAG);
            OFX::ChoiceParamDescriptor* b = OFX::MultiPlane::describeInContextAddChannelChoice(desc, page, clipsForChannels, kParamOutputB, kParamOutputBLabel, kParamOutputBHint);
            b->setDefault(eInputChannelAB);
        } else {
            
            {
                ChoiceParamDescriptor *param = desc.defineChoiceParam(kParamOutputR);
                param->setLabel(kParamOutputRLabel);
                param->setHint(kParamOutputRHint);
                OFX::MultiPlane::addInputChannelOptionsRGBA(param, clipsForChannels, true, 0);
                param->setDefault(eInputChannelAR);
                if (page) {
                    page->addChild(*param);
                }
            }
            {
                ChoiceParamDescriptor *param = desc.defineChoiceParam(kParamOutputG);
                param->setLabel(kParamOutputGLabel);
                param->setHint(kParamOutputGHint);
                OFX::MultiPlane::addInputChannelOptionsRGBA(param, clipsForChannels, true, 0);
                param->setDefault(eInputChannelAG);
                if (page) {
                    page->addChild(*param);
                }
            }
            {
                ChoiceParamDescriptor *param = desc.defineChoiceParam(kParamOutputB);
                param->setLabel(kParamOutputBLabel);
                param->setHint(kParamOutputBHint);
                OFX::MultiPlane::addInputChannelOptionsRGBA(param, clipsForChannels, true, 0);
                param->setDefault(eInputChannelAB);
                if (page) {
                    page->addChild(*param);
                }
            }
        }
        
    }
    // ouputA
    if (gSupportsRGBA || gSupportsAlpha) {
        if (gIsMultiPlanar && gSupportsDynamicChoices) {
            OFX::ChoiceParamDescriptor* a = OFX::MultiPlane::describeInContextAddChannelChoice(desc, page, clipsForChannels, kParamOutputA, kParamOutputALabel, kParamOutputAHint);
            a->setDefault(eInputChannelAA);
        } else {
            {
                ChoiceParamDescriptor *param = desc.defineChoiceParam(kParamOutputA);
                param->setLabel(kParamOutputALabel);
                param->setHint(kParamOutputAHint);
                OFX::MultiPlane::addInputChannelOptionsRGBA(param, clipsForChannels, true, 0);
                param->setDefault(eInputChannelAA);
                if (page) {
                    page->addChild(*param);
                }
            }
        }
        
    }

    // clipInfo
    {
        PushButtonParamDescriptor *param = desc.definePushButtonParam(kParamClipInfo);
        param->setLabel(kParamClipInfoLabel);
        param->setHint(kParamClipInfoHint);
        if (page) {
            page->addChild(*param);
        }
    }
    
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamOutputPremultiplication);
        param->setLabel(kParamOutputPremultiplicationLabel);
        param->setHint(kParamOutputPremultiplicationHint);
        param->setAnimates(false);
        param->appendOption("Opaque");
        param->appendOption("Premultiplied");
        param->appendOption("Unpremultiplied");
        param->setDefault(2);
        if (page) {
            page->addChild(*param);
        }
        desc.addClipPreferencesSlaveParam(*param);
    }
}

OFX::ImageEffect* ShufflePluginFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum context)
{
    return new ShufflePlugin(handle, context);
}


static ShufflePluginFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
mRegisterPluginFactoryInstance(p)

OFXS_NAMESPACE_ANONYMOUS_EXIT
