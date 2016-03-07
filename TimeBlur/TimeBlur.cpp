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
 * OFX TimeBlur plugin.
 */

#include <cmath> // for floor
#include <climits> // for INT_MAX
#include <cassert>
#include <algorithm>
#ifdef DEBUG
#include <cstdio>
#endif

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"

#include "ofxsPixelProcessor.h"
#include "ofxsCoords.h"
#include "ofxsShutter.h"
#include "ofxsMaskMix.h"
#include "ofxsMacros.h"

using namespace OFX;

OFXS_NAMESPACE_ANONYMOUS_ENTER

#define kPluginName "TimeBlurOFX"
#define kPluginGrouping "Time"
#define kPluginDescription \
"Blend frames of the input clip over the shutter range."

#define kPluginIdentifier "net.sf.openfx.TimeBlur"
// History:
// version 1.0: initial version
// version 2.0: use kNatronOfxParamProcess* parameters
#define kPluginVersionMajor 1 // Incrementing this number means that you have broken backwards compatibility of the plug-in.
#define kPluginVersionMinor 0 // Increment this when you have fixed a bug or made it faster.

#define kSupportsTiles 1
#define kSupportsMultiResolution 1
#define kSupportsRenderScale 1
#define kSupportsMultipleClipPARs false
#define kSupportsMultipleClipDepths false
#define kRenderThreadSafety eRenderFullySafe

#define kParamDivisions     "division"
#define kParamDivisionsLabel "Divisions"
#define kParamDivisionsHint  "Number of time samples along the shutter time."

#define kFrameChunk 4 // how many frames to process simultaneously


class TimeBlurProcessorBase : public OFX::PixelProcessor
{
protected:
    std::vector<const OFX::Image*> _srcImgs;
    float *_accumulatorData;
    int _divisions; // 0 for all passes except the last one

public:

    TimeBlurProcessorBase(OFX::ImageEffect &instance)
    : OFX::PixelProcessor(instance)
    , _srcImgs(0)
    , _accumulatorData(0)
    , _divisions(0)
    {
    }

    void setSrcImgs(const std::vector<const OFX::Image*> &v) {_srcImgs = v;}
    void setAccumulator(float *accumulatorData) {_accumulatorData = accumulatorData;}

    void setValues(int divisions)
    {
        _divisions = divisions;
    }
private:
};



template <class PIX, int nComponents, int maxValue>
class TimeBlurProcessor : public TimeBlurProcessorBase
{
public:
    TimeBlurProcessor(OFX::ImageEffect &instance)
    : TimeBlurProcessorBase(instance)
    {
    }

private:

    void multiThreadProcessImages(OfxRectI procWindow)
    {
        assert(1 <= nComponents && nComponents <= 4);
        assert(!_divisions || _dstPixelData);
        float tmpPix[nComponents];
        const float initVal = 0.;
        const bool lastPass = (_divisions != 0);
        for (int y = procWindow.y1; y < procWindow.y2; y++) {
            if (_effect.abort()) {
                break;
            }

            PIX *dstPix = lastPass ? (PIX *) getDstPixelAddress(procWindow.x1, y) : 0;
            assert(!lastPass || dstPix);
            if (lastPass && !dstPix) {
                // coverity[dead_error_line]
                continue;
            }

            for (int x = procWindow.x1; x < procWindow.x2; x++) {
                size_t renderPix = ((_renderWindow.x2 - _renderWindow.x1) * (y - _renderWindow.y1) +
                                    (x - _renderWindow.x1));
                if (_accumulatorData) {
                    std::copy(&_accumulatorData[renderPix * nComponents], &_accumulatorData[renderPix * nComponents + nComponents], tmpPix);
                } else {
                    std::fill(tmpPix, tmpPix + nComponents, initVal);
                }
                // accumulate
                for (unsigned i = 0; i < _srcImgs.size(); ++i) {
                    const PIX *srcPixi = (const PIX *)  (_srcImgs[i] ? _srcImgs[i]->getPixelAddress(x, y) : 0);
                    if (srcPixi) {
                        for (int c = 0; c < nComponents; ++c) {
                            tmpPix[c] += srcPixi[c];
                        }
                    }
                }
                if (!lastPass) {
                    assert(_accumulatorData);
                    if (_accumulatorData) {
                        std::copy(tmpPix, tmpPix + nComponents , &_accumulatorData[renderPix * nComponents]);
                    }
                } else {
                    for (int c = 0; c < nComponents; ++c) {
                        float v = tmpPix[c] / _divisions;
                        dstPix[c] = ofxsClampIfInt<PIX,maxValue>(v, 0, maxValue);
                    }
                    // increment the dst pixel
                    dstPix += nComponents;
                }
            }
        }
    }
};


////////////////////////////////////////////////////////////////////////////////
/** @brief The plugin that does our work */
class TimeBlurPlugin : public OFX::ImageEffect
{
public:
    /** @brief ctor */
    TimeBlurPlugin(OfxImageEffectHandle handle)
    : ImageEffect(handle)
    , _dstClip(0)
    , _srcClip(0)
    , _divisions(0)
    , _shutter(0)
    , _shutteroffset(0)
    , _shuttercustomoffset(0)
    {
        _dstClip = fetchClip(kOfxImageEffectOutputClipName);
        assert(_dstClip && (_dstClip->getPixelComponents() == ePixelComponentAlpha ||
                            _dstClip->getPixelComponents() == ePixelComponentXY ||
                            _dstClip->getPixelComponents() == ePixelComponentRGB ||
                            _dstClip->getPixelComponents() == ePixelComponentRGBA));
        _srcClip = getContext() == OFX::eContextGenerator ? NULL : fetchClip(kOfxImageEffectSimpleSourceClipName);
        assert((!_srcClip && getContext() == OFX::eContextGenerator) ||
               (_srcClip && (_srcClip->getPixelComponents() == ePixelComponentAlpha ||
                             _srcClip->getPixelComponents() == ePixelComponentXY ||
                             _srcClip->getPixelComponents() == ePixelComponentRGB ||
                             _srcClip->getPixelComponents() == ePixelComponentRGBA)));
        _divisions = fetchIntParam(kParamDivisions);
        _shutter = fetchDoubleParam(kParamShutter);
        _shutteroffset = fetchChoiceParam(kParamShutterOffset);
        _shuttercustomoffset = fetchDoubleParam(kParamShutterCustomOffset);
        assert(_divisions && _shutter && _shutteroffset && _shuttercustomoffset);
    }

private:
    /* Override the render */
    virtual void render(const OFX::RenderArguments &args) OVERRIDE FINAL;

    /* set up and run a processor */
    void setupAndProcess(TimeBlurProcessorBase &, const OFX::RenderArguments &args);

    virtual bool isIdentity(const IsIdentityArguments &args, Clip * &identityClip, double &identityTime) OVERRIDE FINAL;

    /** Override the get frames needed action */
    virtual void getFramesNeeded(const OFX::FramesNeededArguments &args, OFX::FramesNeededSetter &frames) OVERRIDE FINAL;

    virtual bool getRegionOfDefinition(const OFX::RegionOfDefinitionArguments &args, OfxRectD &rod) OVERRIDE FINAL;

private:

    template<int nComponents>
    void renderForComponents(const OFX::RenderArguments &args);

    template <class PIX, int nComponents, int maxValue>
    void renderForBitDepth(const OFX::RenderArguments &args);

    // do not need to delete these, the ImageEffect is managing them for us
    OFX::Clip *_dstClip;
    OFX::Clip *_srcClip;
    OFX::IntParam* _divisions;
    OFX::DoubleParam* _shutter;
    OFX::ChoiceParam* _shutteroffset;
    OFX::DoubleParam* _shuttercustomoffset;
};


////////////////////////////////////////////////////////////////////////////////
/** @brief render for the filter */

////////////////////////////////////////////////////////////////////////////////
// basic plugin render function, just a skelington to instantiate templates from

// Since we cannot hold a std::auto_ptr in the vector we must hold a raw pointer.
// To ensure that images are always freed even in case of exceptions, use a RAII class.
struct OptionalImagesHolder_RAII
{
    std::vector<const OFX::Image*> images;
    
    OptionalImagesHolder_RAII()
    : images()
    {
        
    }
    
    ~OptionalImagesHolder_RAII()
    {
        for (unsigned int i = 0; i < images.size(); ++i) {
            delete images[i];
        }
    }
};

/* set up and run a processor */
void
TimeBlurPlugin::setupAndProcess(TimeBlurProcessorBase &processor, const OFX::RenderArguments &args)
{
    const double time = args.time;
    std::auto_ptr<OFX::Image> dst(_dstClip->fetchImage(time));
    if (!dst.get()) {
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
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

    // accumulator image
    std::auto_ptr<OFX::ImageMemory> accumulator;
    float *accumulatorData = NULL;

    // compute range
    double shutter = _shutter->getValueAtTime(time);
    ShutterOffsetEnum shutteroffset = (ShutterOffsetEnum)_shutteroffset->getValueAtTime(time);
    double shuttercustomoffset = _shuttercustomoffset->getValueAtTime(time);
    OfxRangeD range;
    OFX::shutterRange(time, shutter, shutteroffset, shuttercustomoffset, &range);

    int divisions = _divisions->getValueAtTime(time);
    double interval = divisions >= 1 ? (range.max-range.min)/divisions : 1.;

    const OfxRectI& renderWindow = args.renderWindow;
    size_t nPixels = (renderWindow.y2 - renderWindow.y1) * (renderWindow.x2 - renderWindow.x1);

    // Main processing loop.
    // We process the frame range by chunks, to avoid using too much memory.
    int imin;
    int imax = 0;
    const int n = divisions;
    while (imax < n) {
        imin = imax;
        imax = std::min(imin + kFrameChunk, n);
        bool lastPass = (imax == n);

        if (!lastPass) {
            // Initialize accumulator image (always use float)
            if (!accumulatorData) {
                int dstNComponents = _dstClip->getPixelComponentCount();
                accumulator.reset(new OFX::ImageMemory(nPixels * dstNComponents * sizeof(float), this));
                accumulatorData = (float*)accumulator->lock();
                std::fill(accumulatorData, accumulatorData + nPixels * dstNComponents, 0.);
            }
        }

        // fetch the source images
        OptionalImagesHolder_RAII srcImgs;
        for (int i = imin; i < imax; ++i) {
            if (abort()) {
                return;
            }
            const OFX::Image* src = _srcClip ? _srcClip->fetchImage(range.min + i * interval) : 0;
            //std::printf("TimeBlur: fetchimage(%g)\n", range.min + i * interval);
            if (src) {
                if (src->getRenderScale().x != args.renderScale.x ||
                    src->getRenderScale().y != args.renderScale.y ||
                    (src->getField() != OFX::eFieldNone /* for DaVinci Resolve */ && src->getField() != args.fieldToRender)) {
                    setPersistentMessage(OFX::Message::eMessageError, "", "OFX Host gave image with wrong scale or field properties");
                    OFX::throwSuiteStatusException(kOfxStatFailed);
                }
                OFX::BitDepthEnum    srcBitDepth      = src->getPixelDepth();
                OFX::PixelComponentEnum srcComponents = src->getPixelComponents();
                if (srcBitDepth != dstBitDepth || srcComponents != dstComponents) {
                    OFX::throwSuiteStatusException(kOfxStatErrImageFormat);
                }
            }
            srcImgs.images.push_back(src);
        }

        // set the images
        if (lastPass) {
            processor.setDstImg(dst.get());
        }
        processor.setSrcImgs(srcImgs.images);
        // set the render window
        processor.setRenderWindow(renderWindow);
        processor.setAccumulator(accumulatorData);

        processor.setValues(lastPass ? divisions : 0);
        
        // Call the base class process member, this will call the derived templated process code
        processor.process();
    }
}

// the overridden render function
void
TimeBlurPlugin::render(const OFX::RenderArguments &args)
{
    OFX::PixelComponentEnum dstComponents  = _dstClip->getPixelComponents();

    assert(kSupportsMultipleClipPARs   || !_srcClip || _srcClip->getPixelAspectRatio() == _dstClip->getPixelAspectRatio());
    assert(kSupportsMultipleClipDepths || !_srcClip || _srcClip->getPixelDepth()       == _dstClip->getPixelDepth());
    assert(dstComponents == OFX::ePixelComponentAlpha || dstComponents == OFX::ePixelComponentXY || dstComponents == OFX::ePixelComponentRGB || dstComponents == OFX::ePixelComponentRGBA);
    if (dstComponents == OFX::ePixelComponentRGBA) {
        renderForComponents<4>(args);
    } else if (dstComponents == OFX::ePixelComponentAlpha) {
        renderForComponents<1>(args);
    } else if (dstComponents == OFX::ePixelComponentXY) {
        renderForComponents<2>(args);
    } else {
        assert(dstComponents == OFX::ePixelComponentRGB);
        renderForComponents<3>(args);
    }
}

template<int nComponents>
void
TimeBlurPlugin::renderForComponents(const OFX::RenderArguments &args)
{
    OFX::BitDepthEnum dstBitDepth    = _dstClip->getPixelDepth();
    switch (dstBitDepth) {
        case OFX::eBitDepthUByte:
            renderForBitDepth<unsigned char, nComponents, 255>(args);
            break;

        case OFX::eBitDepthUShort:
            renderForBitDepth<unsigned short, nComponents, 65535>(args);
            break;

        case OFX::eBitDepthFloat:
            renderForBitDepth<float, nComponents, 1>(args);
            break;
        default:
            OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
}

template <class PIX, int nComponents, int maxValue>
void
TimeBlurPlugin::renderForBitDepth(const OFX::RenderArguments &args)
{
    TimeBlurProcessor<PIX, nComponents, maxValue> fred(*this);
    setupAndProcess(fred, args);
}

bool
TimeBlurPlugin::isIdentity(const IsIdentityArguments &args, Clip * &identityClip, double &identityTime)
{
    const double time = args.time;

    // compute range
    double shutter = 0.;
    _shutter->getValueAtTime(time, shutter);
    if (shutter != 0) {
        int divisions;
        _divisions->getValueAtTime(time, divisions);
        if (divisions > 1) {
            return false;
        }
    }
    ShutterOffsetEnum shutteroffset_i = (ShutterOffsetEnum)_shutteroffset->getValueAtTime(time);
    double shuttercustomoffset = _shuttercustomoffset->getValueAtTime(time);
    OfxRangeD range;
    OFX::shutterRange(time, shutter, (ShutterOffsetEnum)shutteroffset_i, shuttercustomoffset, &range);

    identityClip = _srcClip;
    identityTime = range.min;
    return true;
}

void
TimeBlurPlugin::getFramesNeeded(const OFX::FramesNeededArguments &args,
                                   OFX::FramesNeededSetter &frames)
{
    const double time = args.time;
    // compute range
    double shutter = _shutter->getValueAtTime(time);
    ShutterOffsetEnum shutteroffset_i = (ShutterOffsetEnum)_shutteroffset->getValueAtTime(time);
    double shuttercustomoffset = _shuttercustomoffset->getValueAtTime(time);
    OfxRangeD range;
    OFX::shutterRange(time, shutter, (ShutterOffsetEnum)shutteroffset_i, shuttercustomoffset, &range);
    int divisions = _divisions->getValueAtTime(time);
    if (shutter == 0 || divisions <= 1) {
        range.max = range.min;
        frames.setFramesNeeded(*_srcClip, range);
        return;
    }
#define OFX_HOST_ACCEPTS_FRACTIONAL_FRAME_RANGES // works with Natron, but this is perhaps borderline with respect to OFX spec
#ifdef OFX_HOST_ACCEPTS_FRACTIONAL_FRAME_RANGES
    //std::printf("TimeBlur: range(%g,%g)\n", range.min, range.max);
    frames.setFramesNeeded(*_srcClip, range);
#else
    double interval = divisions > 1 ? (range.max-range.min)/divisions : 1.;
    for (int i = 1; i < divisions; ++i) {
        double t = range.min + i * interval;

        OfxRangeD r = {t, t};
        //std::printf("TimeBlur: range(%g,%g)\n", r.min, r.max);
        frames.setFramesNeeded(*_srcClip, r);
    }
#endif
}

bool
TimeBlurPlugin::getRegionOfDefinition(const OFX::RegionOfDefinitionArguments &args, OfxRectD &rod)
{
    const double time = args.time;
    // compute range
    double shutter = _shutter->getValueAtTime(time);
    ShutterOffsetEnum shutteroffset = (ShutterOffsetEnum)_shutteroffset->getValueAtTime(time);
    double shuttercustomoffset = _shuttercustomoffset->getValueAtTime(time);
    OfxRangeD range;
    OFX::shutterRange(time, shutter, shutteroffset, shuttercustomoffset, &range);
    int divisions = _divisions->getValueAtTime(time);
    double interval = divisions > 1 ? (range.max-range.min)/divisions : 1.;

    rod = _srcClip->getRegionOfDefinition(range.min);

    for (int i = 1; i < divisions; ++i) {
        OfxRectD srcRoD = _srcClip->getRegionOfDefinition(range.min + i * interval);
        OFX::Coords::rectBoundingBox(srcRoD, rod, &rod);
    }
    return true;
}

mDeclarePluginFactory(TimeBlurPluginFactory, {}, {});

void TimeBlurPluginFactory::describe(OFX::ImageEffectDescriptor &desc)
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

    // set a few flags
    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setSupportsMultiResolution(kSupportsMultiResolution);
    desc.setSupportsTiles(kSupportsTiles);
    desc.setTemporalClipAccess(true);
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipPARs(kSupportsMultipleClipPARs);
    desc.setSupportsMultipleClipDepths(kSupportsMultipleClipDepths);
    desc.setRenderThreadSafety(kRenderThreadSafety);
    
#ifdef OFX_EXTENSIONS_NATRON
    //desc.setChannelSelector(OFX::ePixelComponentNone); // we have our own channel selector
#endif

}

void TimeBlurPluginFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum context)
{
    // Source clip only in the filter context
    // create the mandated source clip
    ClipDescriptor *srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    srcClip->addSupportedComponent(ePixelComponentRGBA);
    srcClip->addSupportedComponent(ePixelComponentRGB);
    srcClip->addSupportedComponent(ePixelComponentXY);
    srcClip->addSupportedComponent(ePixelComponentAlpha);
    srcClip->setTemporalClipAccess(true);
    srcClip->setSupportsTiles(kSupportsTiles);
    srcClip->setIsMask(false);

    // create the mandated output clip
    ClipDescriptor *dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(ePixelComponentRGBA);
    dstClip->addSupportedComponent(ePixelComponentRGB);
    dstClip->addSupportedComponent(ePixelComponentXY);
    dstClip->addSupportedComponent(ePixelComponentAlpha);
    dstClip->setSupportsTiles(kSupportsTiles);

    // make some pages and to things in
    PageParamDescriptor *page = desc.definePageParam("Controls");

    {
        IntParamDescriptor *param = desc.defineIntParam(kParamDivisions);
        param->setLabel(kParamDivisionsLabel);
        param->setHint(kParamDivisionsHint);
        param->setDefault(10);
        param->setRange(1, INT_MAX);
        param->setDisplayRange(1, 10);
        param->setAnimates(true); // can animate
        if (page) {
            page->addChild(*param);
        }
    }

    OFX::shutterDescribeInContext(desc, context, page);
}

OFX::ImageEffect* TimeBlurPluginFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum /*context*/)
{
    return new TimeBlurPlugin(handle);
}


static TimeBlurPluginFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
mRegisterPluginFactoryInstance(p)

OFXS_NAMESPACE_ANONYMOUS_EXIT
