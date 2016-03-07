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
 * OFX Ramp plugin.
 */

#include <cmath>
#include <algorithm>

#include "ofxsProcessing.H"
#include "ofxsCoords.h"
#include "ofxsMaskMix.h"
#include "ofxsMacros.h"
#include "ofxsRamp.h"
#include "ofxNatron.h"

#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

using namespace OFX;

OFXS_NAMESPACE_ANONYMOUS_ENTER

#define kPluginName "RampOFX"
#define kPluginGrouping "Draw"
#define kPluginDescription \
"Draw a ramp between 2 edges.\n" \
"The ramp is composited with the source image using the 'over' operator."
#define kPluginIdentifier "net.sf.openfx.Ramp"
// History:
// version 1.0: initial version
// version 2.0: use kNatronOfxParamProcess* parameters
#define kPluginVersionMajor 2 // Incrementing this number means that you have broken backwards compatibility of the plug-in.
#define kPluginVersionMinor 0 // Increment this when you have fixed a bug or made it faster.

#define kSupportsTiles 1
#define kSupportsMultiResolution 1
#define kSupportsRenderScale 1
#define kSupportsMultipleClipPARs false
#define kSupportsMultipleClipDepths false
#define kRenderThreadSafety eRenderFullySafe


struct RGBAValues {
    double r,g,b,a;
    RGBAValues(double v) : r(v), g(v), b(v), a(v) {}
    RGBAValues() : r(0), g(0), b(0), a(0) {}
};


class RampProcessorBase : public OFX::ImageProcessor
{
   
    
protected:
    const OFX::Image *_srcImg;
    const OFX::Image *_maskImg;
    bool  _doMasking;
    double _mix;
    bool _maskInvert;
    bool _processR;
    bool _processG;
    bool _processB;
    bool _processA;

    RampTypeEnum _type;
    RGBAValues _color0, _color1;
    OfxPointD _point0, _point1;

public:
    RampProcessorBase(OFX::ImageEffect &instance)
    : OFX::ImageProcessor(instance)
    , _srcImg(0)
    , _maskImg(0)
    , _doMasking(false)
    , _mix(1.)
    , _maskInvert(false)
    , _processR(false)
    , _processG(false)
    , _processB(false)
    , _processA(false)
    , _type(eRampTypeLinear)
    {
        _point0.x = _point0.y = _point1.x = _point1.y = 0.;
        _color0.r = _color0.g = _color0.b = _color0.a = 0.;
        _color1.r = _color1.g = _color1.b = _color1.a = 0.;
    }

    /** @brief set the src image */
    void setSrcImg(const OFX::Image *v)
    {
        _srcImg = v;
    }

    void setMaskImg(const OFX::Image *v, bool maskInvert)
    {
        _maskImg = v;
        _maskInvert = maskInvert;
    }

    void doMasking(bool v) {
        _doMasking = v;
    }

    void setValues(RampTypeEnum type,
                   const RGBAValues& color0,
                   const RGBAValues& color1,
                   const OfxPointD& point0,
                   const OfxPointD& point1,
                   double mix,
                   bool processR,
                   bool processG,
                   bool processB,
                   bool processA)
    {
        _type = type;
        _color0 = color0;
        _color1 = color1;
        _point0 = point0;
        _point1 = point1;
        _mix = mix;
        _processR = processR;
        _processG = processG;
        _processB = processB;
        _processA = processA;
    }
 };


template <class PIX, int nComponents, int maxValue>
class RampProcessor : public RampProcessorBase
{
public:
    RampProcessor(OFX::ImageEffect &instance)
    : RampProcessorBase(instance)
    {
    }

private:
    void multiThreadProcessImages(OfxRectI procWindow)
    {
#     ifndef __COVERITY__ // too many coverity[dead_error_line] errors
        const bool r = _processR && (nComponents != 1);
        const bool g = _processG && (nComponents >= 2);
        const bool b = _processB && (nComponents >= 3);
        const bool a = _processA && (nComponents == 1 || nComponents == 4);
        if (r) {
            if (g) {
                if (b) {
                    if (a) {
                        return process<true ,true ,true ,true >(procWindow); // RGBA
                    } else {
                        return process<true ,true ,true ,false>(procWindow); // RGBa
                    }
                } else {
                    if (a) {
                        return process<true ,true ,false,true >(procWindow); // RGbA
                    } else {
                        return process<true ,true ,false,false>(procWindow); // RGba
                    }
                }
            } else {
                if (b) {
                    if (a) {
                        return process<true ,false,true ,true >(procWindow); // RgBA
                    } else {
                        return process<true ,false,true ,false>(procWindow); // RgBa
                    }
                } else {
                    if (a) {
                        return process<true ,false,false,true >(procWindow); // RgbA
                    } else {
                        return process<true ,false,false,false>(procWindow); // Rgba
                    }
                }
            }
        } else {
            if (g) {
                if (b) {
                    if (a) {
                        return process<false,true ,true ,true >(procWindow); // rGBA
                    } else {
                        return process<false,true ,true ,false>(procWindow); // rGBa
                    }
                } else {
                    if (a) {
                        return process<false,true ,false,true >(procWindow); // rGbA
                    } else {
                        return process<false,true ,false,false>(procWindow); // rGba
                    }
                }
            } else {
                if (b) {
                    if (a) {
                        return process<false,false,true ,true >(procWindow); // rgBA
                    } else {
                        return process<false,false,true ,false>(procWindow); // rgBa
                    }
                } else {
                    if (a) {
                        return process<false,false,false,true >(procWindow); // rgbA
                    } else {
                        return process<false,false,false,false>(procWindow); // rgba
                    }
                }
            }
        }
#     endif
    }

    template<bool processR, bool processG, bool processB, bool processA>
    void process(const OfxRectI& procWindow)
    {
        assert((!processR && !processG && !processB) || (nComponents == 3 || nComponents == 4));
        assert(!processA || (nComponents == 1 || nComponents == 4));
        switch (_type) {
            case eRampTypeLinear:
                processForType<processR,processG,processB,processA,eRampTypeLinear>(procWindow);
                break;
            case eRampTypePLinear:
                processForType<processR,processG,processB,processA,eRampTypePLinear>(procWindow);
                break;
            case eRampTypeEaseIn:
                processForType<processR,processG,processB,processA,eRampTypeEaseIn>(procWindow);
                break;
            case eRampTypeEaseOut:
                processForType<processR,processG,processB,processA,eRampTypeEaseOut>(procWindow);
                break;
            case eRampTypeSmooth:
                processForType<processR,processG,processB,processA,eRampTypeSmooth>(procWindow);
                break;
            case eRampTypeNone:
                processForType<processR,processG,processB,processA,eRampTypeNone>(procWindow);
                break;
        }
    }
    
    
    template<bool processR, bool processG, bool processB, bool processA, RampTypeEnum type>
    void processForType(const OfxRectI& procWindow)
    {
        float tmpPix[4];

        const double norm2 = (_point1.x - _point0.x)*(_point1.x - _point0.x) + (_point1.y - _point0.y)*(_point1.y - _point0.y);
        const double nx = norm2 == 0. ? 0. : (_point1.x - _point0.x)/ norm2;
        const double ny = norm2 == 0. ? 0. : (_point1.y - _point0.y)/ norm2;

        for (int y = procWindow.y1; y < procWindow.y2; ++y) {
            if (_effect.abort()) {
                break;
            }
            
            PIX *dstPix = (PIX *) _dstImg->getPixelAddress(procWindow.x1, y);
            
            for (int x = procWindow.x1; x < procWindow.x2; ++x, dstPix += nComponents) {
                const PIX *srcPix = (const PIX *)  (_srcImg ? _srcImg->getPixelAddress(x, y) : 0);
                OfxPointI p_pixel;
                OfxPointD p;
                p_pixel.x = x;
                p_pixel.y = y;
                OFX::Coords::toCanonical(p_pixel, _dstImg->getRenderScale(), _dstImg->getPixelAspectRatio(), &p);

                double t = ofxsRampFunc<type>(_point0, nx, ny, p);
                    
                tmpPix[0] = (float)_color0.r * (1 - (float)t) + (float)_color1.r * (float)t;
                tmpPix[1] = (float)_color0.g * (1 - (float)t) + (float)_color1.g * (float)t;
                tmpPix[2] = (float)_color0.b * (1 - (float)t) + (float)_color1.b * (float)t;
                tmpPix[3] = (float)_color0.a * (1 - (float)t) + (float)_color1.a * (float)t;

                float a = tmpPix[3];

                // ofxsMaskMixPix takes non-normalized values
                tmpPix[0] *= maxValue;
                tmpPix[1] *= maxValue;
                tmpPix[2] *= maxValue;
                tmpPix[3] *= maxValue;
                float srcPixRGBA[4] = {0, 0, 0, 0};
                if (srcPix) {
                    if (nComponents >= 3) {
                        srcPixRGBA[0] = srcPix[0];
                        srcPixRGBA[1] = srcPix[1];
                        srcPixRGBA[2] = srcPix[2];
                    }
                    if (nComponents == 1 || nComponents == 4) {
                        srcPixRGBA[3] = srcPix[nComponents-1];
                    }
                }
                if (processR) {
                    tmpPix[0] = tmpPix[0] + srcPixRGBA[0]*(1.f-a);
                } else {
                    tmpPix[0] = srcPixRGBA[0];
                }
                if (processG) {
                    tmpPix[1] = tmpPix[1] + srcPixRGBA[1]*(1.f-a);
                } else {
                    tmpPix[1] = srcPixRGBA[1];
                }
                if (processB) {
                    tmpPix[2] = tmpPix[2] + srcPixRGBA[2]*(1.f-a);
                } else {
                    tmpPix[2] = srcPixRGBA[2];
                }
                if (processA) {
                    tmpPix[3] = tmpPix[3] + srcPixRGBA[3]*(1.f-a);
                } else {
                    tmpPix[3] = srcPixRGBA[3];
                }
                ofxsMaskMixPix<PIX, nComponents, maxValue, true>(tmpPix, x, y, srcPix, _doMasking, _maskImg, (float)_mix, _maskInvert, dstPix);
            }
        }
    }

};





////////////////////////////////////////////////////////////////////////////////
/** @brief The plugin that does our work */
class RampPlugin : public OFX::ImageEffect
{
public:
    /** @brief ctor */
    RampPlugin(OfxImageEffectHandle handle)
    : ImageEffect(handle)
    , _dstClip(0)
    , _srcClip(0)
    , _processR(0)
    , _processG(0)
    , _processB(0)
    , _processA(0)
    , _point0(0)
    , _color0(0)
    , _point1(0)
    , _color1(0)
    , _type(0)
    , _interactive(0)
    , _mix(0)
    , _maskApply(0)
    , _maskInvert(0)
    {
        _dstClip = fetchClip(kOfxImageEffectOutputClipName);
        assert(_dstClip && (_dstClip->getPixelComponents() == ePixelComponentAlpha ||
                            _dstClip->getPixelComponents() == ePixelComponentRGB ||
                            _dstClip->getPixelComponents() == ePixelComponentRGBA));
        _srcClip = getContext() == OFX::eContextGenerator ? NULL : fetchClip(kOfxImageEffectSimpleSourceClipName);
        assert((!_srcClip && getContext() == OFX::eContextGenerator) ||
               (_srcClip && (_srcClip->getPixelComponents() == ePixelComponentAlpha ||
                             _srcClip->getPixelComponents() == ePixelComponentRGB ||
                             _srcClip->getPixelComponents() == ePixelComponentRGBA)));
        _maskClip = fetchClip(getContext() == OFX::eContextPaint ? "Brush" : "Mask");
        assert(!_maskClip || _maskClip->getPixelComponents() == ePixelComponentAlpha);
        
        _processR = fetchBooleanParam(kNatronOfxParamProcessR);
        _processG = fetchBooleanParam(kNatronOfxParamProcessG);
        _processB = fetchBooleanParam(kNatronOfxParamProcessB);
        _processA = fetchBooleanParam(kNatronOfxParamProcessA);
        assert(_processR && _processG && _processB && _processA);
        _point0 = fetchDouble2DParam(kParamRampPoint0Old);
        _point1 = fetchDouble2DParam(kParamRampPoint1Old);
        _color0 = fetchRGBAParam(kParamRampColor0Old);
        _color1 = fetchRGBAParam(kParamRampColor1Old);
        _type = fetchChoiceParam(kParamRampTypeOld);
        _interactive = fetchBooleanParam(kParamRampInteractiveOld);
        assert(_point0 && _point1 && _color0 && _color1 && _type && _interactive);

        _mix = fetchDoubleParam(kParamMix);
        _maskApply = paramExists(kParamMaskApply) ? fetchBooleanParam(kParamMaskApply) : 0;
        _maskInvert = fetchBooleanParam(kParamMaskInvert);
        assert(_mix && _maskInvert);

        // update Visibility
        OFX::InstanceChangedArgs args = {eChangeUserEdit, 0., {0., 0.}};
        changedParam(args, kParamRampType);
    }
    
private:
    /* override is identity */
    virtual bool isIdentity(const OFX::IsIdentityArguments &args, OFX::Clip * &identityClip, double &identityTime) OVERRIDE FINAL;

    /* Override the clip preferences */
    void getClipPreferences(OFX::ClipPreferencesSetter &clipPreferences) OVERRIDE FINAL;

    /* Override the render */
    virtual void render(const OFX::RenderArguments &args) OVERRIDE FINAL;

    virtual void changedParam(const OFX::InstanceChangedArgs &args, const std::string &paramName) OVERRIDE FINAL;

    template <int nComponents>
    void renderInternal(const OFX::RenderArguments &args, OFX::BitDepthEnum dstBitDepth);

    /* set up and run a processor */
    void setupAndProcess(RampProcessorBase &, const OFX::RenderArguments &args);

private:
    
    // do not need to delete these, the ImageEffect is managing them for us
    Clip *_dstClip;
    Clip *_srcClip;
    Clip *_maskClip;

    BooleanParam* _processR;
    BooleanParam* _processG;
    BooleanParam* _processB;
    BooleanParam* _processA;
    Double2DParam* _point0;
    RGBAParam* _color0;
    Double2DParam* _point1;
    RGBAParam* _color1;
    ChoiceParam* _type;
    BooleanParam* _interactive;
    OFX::DoubleParam* _mix;
    OFX::BooleanParam* _maskApply;
    OFX::BooleanParam* _maskInvert;
};

////////////////////////////////////////////////////////////////////////////////
/** @brief render for the filter */

////////////////////////////////////////////////////////////////////////////////
// basic plugin render function, just a skelington to instantiate templates from

/* set up and run a processor */
void
RampPlugin::setupAndProcess(RampProcessorBase &processor, const OFX::RenderArguments &args)
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
    std::auto_ptr<const OFX::Image> src((_srcClip && _srcClip->isConnected()) ?
                                        _srcClip->fetchImage(args.time) : 0);
    if (src.get()) {
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

    // auto ptr for the mask.
    bool doMasking = ((!_maskApply || _maskApply->getValueAtTime(args.time)) && _maskClip && _maskClip->isConnected());
    std::auto_ptr<const OFX::Image> mask(doMasking ? _maskClip->fetchImage(args.time) : 0);
    if (doMasking) {
        if (mask.get()) {
            if (mask->getRenderScale().x != args.renderScale.x ||
                mask->getRenderScale().y != args.renderScale.y ||
                (mask->getField() != OFX::eFieldNone /* for DaVinci Resolve */ && mask->getField() != args.fieldToRender)) {
                setPersistentMessage(OFX::Message::eMessageError, "", "OFX Host gave image with wrong scale or field properties");
                OFX::throwSuiteStatusException(kOfxStatFailed);
            }
        }
        bool maskInvert;
        _maskInvert->getValueAtTime(args.time, maskInvert);
        processor.doMasking(true);
        processor.setMaskImg(mask.get(), maskInvert);
    }

    if (src.get() && dst.get()) {
        OFX::BitDepthEnum    srcBitDepth      = src->getPixelDepth();
        OFX::PixelComponentEnum srcComponents = src->getPixelComponents();
        OFX::BitDepthEnum dstBitDepth       = dst->getPixelDepth();
        OFX::PixelComponentEnum dstComponents  = dst->getPixelComponents();

        // see if they have the same depths and bytes and all
        if (srcBitDepth != dstBitDepth || srcComponents != dstComponents)
            OFX::throwSuiteStatusException(kOfxStatErrImageFormat);
    }

    // set the images
    processor.setDstImg(dst.get());
    processor.setSrcImg(src.get());

    // set the render window
    processor.setRenderWindow(args.renderWindow);

    RampTypeEnum type = (RampTypeEnum)_type->getValueAtTime(time);
    
    OfxPointD point0,point1;
    _point0->getValueAtTime(args.time, point0.x, point0.y);
    _point1->getValueAtTime(args.time, point1.x, point1.y);
    
    RGBAValues color0,color1;
    _color0->getValueAtTime(args.time, color0.r, color0.g, color0.b, color0.a);
    _color1->getValueAtTime(args.time, color1.r, color1.g, color1.b, color1.a);
    
    bool processR, processG, processB, processA;
    _processR->getValueAtTime(time, processR);
    _processG->getValueAtTime(time, processG);
    _processB->getValueAtTime(time, processB);
    _processA->getValueAtTime(time, processA);

    double mix;
    _mix->getValueAtTime(args.time, mix);

    processor.setValues(type,
                        color0, color1,
                        point0, point1,
                        mix,
                        processR, processG, processB, processA);
    // Call the base class process member, this will call the derived templated process code
    processor.process();
}


// the internal render function
template <int nComponents>
void
RampPlugin::renderInternal(const OFX::RenderArguments &args, OFX::BitDepthEnum dstBitDepth)
{
    switch (dstBitDepth) {
        case OFX::eBitDepthUByte: {
            RampProcessor<unsigned char, nComponents, 255> fred(*this);
            setupAndProcess(fred, args);
        }   break;
        case OFX::eBitDepthUShort: {
            RampProcessor<unsigned short, nComponents, 65535> fred(*this);
            setupAndProcess(fred, args);
        }   break;
        case OFX::eBitDepthFloat: {
            RampProcessor<float, nComponents, 1> fred(*this);
            setupAndProcess(fred, args);
        }   break;
        default:
            OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
}

// the overridden render function
void
RampPlugin::render(const OFX::RenderArguments &args)
{
    
    // instantiate the render code based on the pixel depth of the dst clip
    OFX::BitDepthEnum       dstBitDepth    = _dstClip->getPixelDepth();
    OFX::PixelComponentEnum dstComponents  = _dstClip->getPixelComponents();

    assert(kSupportsMultipleClipPARs   || !_srcClip || _srcClip->getPixelAspectRatio() == _dstClip->getPixelAspectRatio());
    assert(kSupportsMultipleClipDepths || !_srcClip || _srcClip->getPixelDepth()       == _dstClip->getPixelDepth());
    assert(dstComponents == OFX::ePixelComponentRGBA || dstComponents == OFX::ePixelComponentRGB || dstComponents == OFX::ePixelComponentXY || dstComponents == OFX::ePixelComponentAlpha);
    if (dstComponents == OFX::ePixelComponentRGBA) {
        renderInternal<4>(args, dstBitDepth);
    } else if (dstComponents == OFX::ePixelComponentRGB) {
        renderInternal<3>(args, dstBitDepth);
    } else if (dstComponents == OFX::ePixelComponentXY) {
        renderInternal<2>(args, dstBitDepth);
    } else {
        assert(dstComponents == OFX::ePixelComponentAlpha);
        renderInternal<1>(args, dstBitDepth);
    }
}

bool
RampPlugin::isIdentity(const OFX::IsIdentityArguments &args,
                       OFX::Clip * &identityClip,
                       double &/*identityTime*/)
{
    double mix;
    _mix->getValueAtTime(args.time, mix);

    if (mix == 0. /*|| (!processR && !processG && !processB && !processA)*/) {
        identityClip = _srcClip;
        return true;
    }

    {
        bool processR;
        bool processG;
        bool processB;
        bool processA;
        _processR->getValueAtTime(args.time, processR);
        _processG->getValueAtTime(args.time, processG);
        _processB->getValueAtTime(args.time, processB);
        _processA->getValueAtTime(args.time, processA);
        if (!processR && !processG && !processB && !processA) {
            identityClip = _srcClip;
            return true;
        }
    }
    
    RGBAValues color0,color1;
    _color0->getValueAtTime(args.time, color0.r, color0.g, color0.b, color0.a);
    _color1->getValueAtTime(args.time, color1.r, color1.g, color1.b, color1.a);
    if (color0.a == 0. && color1.a == 0.) {
        identityClip = _srcClip;
        return true;
    }

    bool doMasking = ((!_maskApply || _maskApply->getValueAtTime(args.time)) && _maskClip && _maskClip->isConnected());
    if (doMasking) {
        bool maskInvert;
        _maskInvert->getValueAtTime(args.time, maskInvert);
        if (!maskInvert) {
            OfxRectI maskRoD;
            OFX::Coords::toPixelEnclosing(_maskClip->getRegionOfDefinition(args.time), args.renderScale, _maskClip->getPixelAspectRatio(), &maskRoD);
            // effect is identity if the renderWindow doesn't intersect the mask RoD
            if (!OFX::Coords::rectIntersection<OfxRectI>(args.renderWindow, maskRoD, 0)) {
                identityClip = _srcClip;
                return true;
            }
        }
    }

    return false;
}

/* Override the clip preferences */
void
RampPlugin::getClipPreferences(OFX::ClipPreferencesSetter &clipPreferences)
{
    if (_srcClip) {
        // set the premultiplication of _dstClip if alpha is affected and source is Opaque
        bool processA;
        _processA->getValue(processA);
        if (processA &&
            (_dstClip->getPixelComponents() == ePixelComponentRGBA ||
             _dstClip->getPixelComponents() == ePixelComponentAlpha) &&
            _srcClip->getPreMultiplication() == eImageOpaque) {
            clipPreferences.setOutputPremultiplication(eImageUnPreMultiplied);
        }
    }
}

void
RampPlugin::changedParam(const OFX::InstanceChangedArgs &args,
                         const std::string &paramName)
{
    if (paramName == kParamRampType && args.reason == OFX::eChangeUserEdit) {
        RampTypeEnum type = (RampTypeEnum)_type->getValueAtTime(args.time);
        bool noramp = (type == eRampTypeNone);
        _color0->setIsSecret(noramp);
        _point0->setIsSecret(noramp);
        _point1->setIsSecret(noramp);
        _interactive->setIsSecret(noramp);
        _color0->setEnabled(!noramp);
        _point0->setEnabled(!noramp);
        _point1->setEnabled(!noramp);
        _interactive->setEnabled(!noramp);
    }
}


//static void intersectToRoD(const OfxRectD& rod,const OfxPointD& p0)



mDeclarePluginFactory(RampPluginFactory, {}, {});

void RampPluginFactory::describe(OFX::ImageEffectDescriptor &desc)
{
    // basic labels
    desc.setLabel(kPluginName);
    desc.setPluginGrouping(kPluginGrouping);
    desc.setPluginDescription(kPluginDescription);

    desc.addSupportedContext(eContextGeneral);
    desc.addSupportedContext(eContextGenerator);

    desc.addSupportedBitDepth(eBitDepthUByte);
    desc.addSupportedBitDepth(eBitDepthUShort);
    desc.addSupportedBitDepth(eBitDepthFloat);
    
    
    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setTemporalClipAccess(false);
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipPARs(kSupportsMultipleClipPARs);
    desc.setSupportsMultipleClipDepths(kSupportsMultipleClipDepths);
    desc.setRenderThreadSafety(kRenderThreadSafety);
    
    desc.setSupportsTiles(kSupportsTiles);
    
    // in order to support multiresolution, render() must take into account the pixelaspectratio and the renderscale
    // and scale the transform appropriately.
    // All other functions are usually in canonical coordinates.
    desc.setSupportsMultiResolution(kSupportsMultiResolution);
    desc.setOverlayInteractDescriptor(new RampOverlayDescriptorOldParams);
    
#ifdef OFX_EXTENSIONS_NATRON
    desc.setChannelSelector(OFX::ePixelComponentNone); // we have our own channel selector
#endif

}



OFX::ImageEffect* RampPluginFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum /*context*/)
{
    return new RampPlugin(handle);
}




void RampPluginFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum context)
{
    // Source clip only in the filter context
    // create the mandated source clip
    // always declare the source clip first, because some hosts may consider
    // it as the default input clip (e.g. Nuke)
    ClipDescriptor *srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    srcClip->addSupportedComponent(ePixelComponentRGBA);
    srcClip->addSupportedComponent(ePixelComponentRGB);
    srcClip->addSupportedComponent(ePixelComponentXY);
    srcClip->addSupportedComponent(ePixelComponentAlpha);
    srcClip->setTemporalClipAccess(false);
    srcClip->setSupportsTiles(kSupportsTiles);
    srcClip->setIsMask(false);
    srcClip->setOptional(true);

    // create the mandated output clip
    ClipDescriptor *dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(ePixelComponentRGBA);
    dstClip->addSupportedComponent(ePixelComponentRGB);
    dstClip->addSupportedComponent(ePixelComponentXY);
    dstClip->addSupportedComponent(ePixelComponentAlpha);
    dstClip->setSupportsTiles(kSupportsTiles);

    ClipDescriptor *maskClip = (context == eContextPaint) ? desc.defineClip("Brush") : desc.defineClip("Mask");
    maskClip->addSupportedComponent(ePixelComponentAlpha);
    maskClip->setTemporalClipAccess(false);
    if (context != eContextPaint) {
        maskClip->setOptional(true);
    }
    maskClip->setSupportsTiles(kSupportsTiles);
    maskClip->setIsMask(true);

    // make some pages and to things in
    PageParamDescriptor *page = desc.definePageParam("Controls");
    
    {
        OFX::BooleanParamDescriptor* param = desc.defineBooleanParam(kNatronOfxParamProcessR);
        param->setLabel(kNatronOfxParamProcessRLabel);
        param->setHint(kNatronOfxParamProcessRHint);
        param->setDefault(true);
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        OFX::BooleanParamDescriptor* param = desc.defineBooleanParam(kNatronOfxParamProcessG);
        param->setLabel(kNatronOfxParamProcessGLabel);
        param->setHint(kNatronOfxParamProcessGHint);
        param->setDefault(true);
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        OFX::BooleanParamDescriptor* param = desc.defineBooleanParam(kNatronOfxParamProcessB);
        param->setLabel(kNatronOfxParamProcessBLabel);
        param->setHint(kNatronOfxParamProcessBHint);
        param->setDefault(true);
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        OFX::BooleanParamDescriptor* param = desc.defineBooleanParam(kNatronOfxParamProcessA);
        param->setLabel(kNatronOfxParamProcessALabel);
        param->setHint(kNatronOfxParamProcessAHint);
        param->setDefault(true);
        param->setAnimates(false);
        desc.addClipPreferencesSlaveParam(*param);
        if (page) {
            page->addChild(*param);
        }
    }

    ofxsRampDescribeParams(desc, page, NULL, eRampTypeLinear, /*isOpen=*/true, /*oldParams=*/true);

    ofxsMaskMixDescribeParams(desc, page);
}


static RampPluginFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
mRegisterPluginFactoryInstance(p)

OFXS_NAMESPACE_ANONYMOUS_EXIT
