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
 * OFX Position plugin.
 */

#include <cmath>
#include <iostream>
#ifdef _WINDOWS
#include <windows.h>
#endif

#include "ofxsCoords.h"
#include "ofxsMacros.h"
#include "ofxsCopier.h"
#include "ofxsPositionInteract.h"

using namespace OFX;

OFXS_NAMESPACE_ANONYMOUS_ENTER

#define kPluginName "PositionOFX"
#define kPluginGrouping "Transform"
#define kPluginDescription "Translate an image by an integer number of pixels.\n"\
"This plugin does not concatenate transforms."
#define kPluginIdentifier "net.sf.openfx.Position"
#define kPluginVersionMajor 1 // Incrementing this number means that you have broken backwards compatibility of the plug-in.
#define kPluginVersionMinor 0 // Increment this when you have fixed a bug or made it faster.

#define kSupportsTiles 1
#define kSupportsMultiResolution 1
#define kSupportsRenderScale 1
#define kSupportsMultipleClipPARs false
#define kSupportsMultipleClipDepths true
#define kRenderThreadSafety eRenderFullySafe

#define kParamTranslate "translate"
#define kParamTranslateLabel "Translate"
#define kParamTranslateHint "New position of the bottom-left pixel. Rounded to the closest pixel."

#define kParamInteractive "interactive"
#define kParamInteractiveLabel "Interactive"
#define kParamInteractiveHint \
"When checked the image will be rendered whenever moving the overlay interact instead of when releasing the mouse button."


////////////////////////////////////////////////////////////////////////////////
/** @brief The plugin that does our work */
class PositionPlugin : public OFX::ImageEffect
{
public:
    /** @brief ctor */
    PositionPlugin(OfxImageEffectHandle handle)
    : ImageEffect(handle)
    , _dstClip(0)
    , _srcClip(0)
    , _translate(0)
    {
        _dstClip = fetchClip(kOfxImageEffectOutputClipName);
        _srcClip = getContext() == OFX::eContextGenerator ? NULL : fetchClip(kOfxImageEffectSimpleSourceClipName);
        _translate = fetchDouble2DParam(kParamTranslate);
        assert(_translate);
    }

private:
    virtual void render(const OFX::RenderArguments &args) OVERRIDE FINAL;

    virtual bool isIdentity(const IsIdentityArguments &args, Clip * &identityClip, double &identityTime) OVERRIDE FINAL;

    // override the rod call
    virtual bool getRegionOfDefinition(const OFX::RegionOfDefinitionArguments &args, OfxRectD &rod) OVERRIDE FINAL;

    // override the roi call
    virtual void getRegionsOfInterest(const OFX::RegionsOfInterestArguments &args, OFX::RegionOfInterestSetter &rois) OVERRIDE FINAL;

private:
    // do not need to delete these, the ImageEffect is managing them for us
    OFX::Clip *_dstClip;
    OFX::Clip *_srcClip;

    Double2DParam* _translate;
};

// the overridden render function
void
PositionPlugin::render(const OFX::RenderArguments &args)
{
    assert(kSupportsMultipleClipPARs   || !_srcClip || _srcClip->getPixelAspectRatio() == _dstClip->getPixelAspectRatio());
    assert(kSupportsMultipleClipDepths || !_srcClip || _srcClip->getPixelDepth()       == _dstClip->getPixelDepth());

    // do the rendering
    std::auto_ptr<OFX::Image> dst(_dstClip->fetchImage(args.time));
    if (!dst.get()) {
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    if (dst->getRenderScale().x != args.renderScale.x ||
        dst->getRenderScale().y != args.renderScale.y ||
        (dst->getField() != OFX::eFieldNone /* for DaVinci Resolve */ && dst->getField() != args.fieldToRender)) {
        setPersistentMessage(OFX::Message::eMessageError, "", "OFX Host gave image with wrong scale or field properties");
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    void* dstPixelData;
    OfxRectI dstBounds;
    OFX::PixelComponentEnum dstComponents;
    OFX::BitDepthEnum dstBitDepth;
    int dstRowBytes;
    getImageData(dst.get(), &dstPixelData, &dstBounds, &dstComponents, &dstBitDepth, &dstRowBytes);
    int dstPixelComponentCount = dst->getPixelComponentCount();

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
    const void* srcPixelData;
    OfxRectI srcBounds;
    OFX::PixelComponentEnum srcPixelComponents;
    OFX::BitDepthEnum srcBitDepth;
    int srcRowBytes;
    getImageData(src.get(), &srcPixelData, &srcBounds, &srcPixelComponents, &srcBitDepth, &srcRowBytes);
    int srcPixelComponentCount = src->getPixelComponentCount();

    // translate srcBounds
    const double time = args.time;
    double par = _dstClip->getPixelAspectRatio();
    OfxPointD t_canonical;
    _translate->getValueAtTime(time, t_canonical.x, t_canonical.y);
    OfxPointI t_pixel;

    // rounding is done by going to pixels, and back to Canonical
    t_pixel.x = (int)std::floor(t_canonical.x * args.renderScale.x / par + 0.5);
    t_pixel.y = (int)std::floor(t_canonical.y * args.renderScale.y + 0.5);
    if (args.fieldToRender == eFieldBoth) {
        // round to an even y
        t_pixel.y = t_pixel.y - (t_pixel.y & 1);
    }

    // translate srcBounds
    srcBounds.x1 += t_pixel.x;
    srcBounds.x2 += t_pixel.x;
    srcBounds.y1 += t_pixel.y;
    srcBounds.y2 += t_pixel.y;

    copyPixels(*this, args.renderWindow, srcPixelData, srcBounds, srcPixelComponents, srcPixelComponentCount, srcBitDepth, srcRowBytes, dstPixelData, dstBounds, dstComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
}

// override the rod call
bool
PositionPlugin::getRegionOfDefinition(const OFX::RegionOfDefinitionArguments &args, OfxRectD &rod)
{
    if (!_srcClip) {
        return false;
    }
    const double time = args.time;
    OfxRectD srcrod = _srcClip->getRegionOfDefinition(time);
    if (OFX::Coords::rectIsEmpty(srcrod)) {
        return false;
    }
    double par = _dstClip->getPixelAspectRatio();
    OfxPointD t_canonical;
    _translate->getValueAtTime(time, t_canonical.x, t_canonical.y);
    OfxPointI t_pixel;

    // rounding is done by going to pixels, and back to Canonical
    t_pixel.x = (int)std::floor(t_canonical.x * args.renderScale.x / par + 0.5);
    t_pixel.y = (int)std::floor(t_canonical.y * args.renderScale.y + 0.5);
    if (_srcClip->getFieldOrder() == eFieldBoth) {
        // round to an even y
        t_pixel.y = t_pixel.y - (t_pixel.y & 1);
    }
    if (t_pixel.x == 0 && t_pixel.y == 0) {
        return false;
    }
    t_canonical.x = t_pixel.x * par / args.renderScale.x;
    t_canonical.y = t_pixel.y / args.renderScale.y;
    rod.x1 = srcrod.x1 + t_canonical.x;
    rod.x2 = srcrod.x2 + t_canonical.x;
    rod.y1 = srcrod.y1 + t_canonical.y;
    rod.y2 = srcrod.y2 + t_canonical.y;

    return true;
}

// override the roi call
void
PositionPlugin::getRegionsOfInterest(const OFX::RegionsOfInterestArguments &args, OFX::RegionOfInterestSetter &rois)
{
    if (!_srcClip) {
        return;
    }
    const double time = args.time;
    OfxRectD srcRod = _srcClip->getRegionOfDefinition(time);
    if (OFX::Coords::rectIsEmpty(srcRod)) {
        return;
    }
    double par = _dstClip->getPixelAspectRatio();
    OfxPointD t_canonical;
    _translate->getValueAtTime(time, t_canonical.x, t_canonical.y);
    OfxPointI t_pixel;

    // rounding is done by going to pixels, and back to Canonical
    t_pixel.x = (int)std::floor(t_canonical.x * args.renderScale.x / par + 0.5);
    t_pixel.y = (int)std::floor(t_canonical.y * args.renderScale.y + 0.5);
    if (_srcClip->getFieldOrder() == eFieldBoth) {
        // round to an even y
        t_pixel.y = t_pixel.y - (t_pixel.y & 1);
    }
    if (t_pixel.x == 0 && t_pixel.y == 0) {
        return;
    }
    t_canonical.x = t_pixel.x * par / args.renderScale.x;
    t_canonical.y = t_pixel.y / args.renderScale.y;

    OfxRectD srcRoi = args.regionOfInterest;
    srcRoi.x1 -= t_canonical.x;
    srcRoi.x2 -= t_canonical.x;
    srcRoi.y1 -= t_canonical.y;
    srcRoi.y2 -= t_canonical.y;
    // intersect srcRoi with srcRoD
    OFX::Coords::rectIntersection(srcRoi, srcRod, &srcRoi);
    rois.setRegionOfInterest(*_srcClip, srcRoi);
}

// overridden is identity
bool
PositionPlugin::isIdentity(const IsIdentityArguments &args, Clip * &identityClip, double &/*identityTime*/)
{
    const double time = args.time;
    double par = _dstClip->getPixelAspectRatio();
    OfxPointD t_canonical;
    _translate->getValueAtTime(time, t_canonical.x, t_canonical.y);
    OfxPointI t_pixel;

    // rounding is done by going to pixels, and back to Canonical
    t_pixel.x = (int)std::floor(t_canonical.x * args.renderScale.x / par + 0.5);
    t_pixel.y = (int)std::floor(t_canonical.y * args.renderScale.y + 0.5);
    if (args.fieldToRender == eFieldBoth) {
        // round to an even y
        t_pixel.y = t_pixel.y - (t_pixel.y & 1);
    }
    if (t_pixel.x == 0 && t_pixel.y == 0) {
        identityClip = _srcClip;
        return true;
    }

    return false;
}



mDeclarePluginFactory(PositionPluginFactory, {}, {});

struct PositionInteractParam {
    static const char *name() { return kParamTranslate; }
    static const char *interactiveName() { return kParamInteractive; }
};

void PositionPluginFactory::describe(OFX::ImageEffectDescriptor &desc)
{
    // basic labels
    desc.setLabel(kPluginName);
    desc.setPluginGrouping(kPluginGrouping);
    desc.setPluginDescription(kPluginDescription);

    // add the supported contexts, only filter at the moment
    desc.addSupportedContext(eContextFilter);

    // add supported pixel depths
    desc.addSupportedBitDepth(eBitDepthNone);
    desc.addSupportedBitDepth(eBitDepthUByte);
    desc.addSupportedBitDepth(eBitDepthUShort);
    desc.addSupportedBitDepth(eBitDepthHalf);
    desc.addSupportedBitDepth(eBitDepthFloat);
    desc.addSupportedBitDepth(eBitDepthCustom);
#ifdef OFX_EXTENSIONS_VEGAS
    desc.addSupportedBitDepth(eBitDepthUByteBGRA);
    desc.addSupportedBitDepth(eBitDepthUShortBGRA);
    desc.addSupportedBitDepth(eBitDepthFloatBGRA);
#endif

    // set a few flags
    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setSupportsMultiResolution(kSupportsMultiResolution);
    desc.setSupportsTiles(kSupportsTiles);
    desc.setTemporalClipAccess(false);
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipPARs(kSupportsMultipleClipPARs);
    desc.setSupportsMultipleClipDepths(kSupportsMultipleClipDepths);
    desc.setRenderThreadSafety(kRenderThreadSafety);

    desc.setOverlayInteractDescriptor(new PositionOverlayDescriptor<PositionInteractParam>);
#ifdef OFX_EXTENSIONS_NUKE
    // ask the host to render all planes
    desc.setPassThroughForNotProcessedPlanes(ePassThroughLevelRenderAllRequestedPlanes);
#endif
#ifdef OFX_EXTENSIONS_NATRON
    desc.setChannelSelector(ePixelComponentNone);
#endif
}

void PositionPluginFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum /*context*/)
{
    // Source clip only in the filter context
    // create the mandated source clip
    ClipDescriptor *srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    srcClip->addSupportedComponent(ePixelComponentNone);
    srcClip->addSupportedComponent(ePixelComponentRGBA);
    srcClip->addSupportedComponent(ePixelComponentRGB);
    srcClip->addSupportedComponent(ePixelComponentAlpha);
#ifdef OFX_EXTENSIONS_NATRON
    srcClip->addSupportedComponent(ePixelComponentXY);
#endif
    srcClip->setTemporalClipAccess(false);
    srcClip->setSupportsTiles(kSupportsTiles);
    srcClip->setIsMask(false);

    // create the mandated output clip
    ClipDescriptor *dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(ePixelComponentNone);
    dstClip->addSupportedComponent(ePixelComponentRGBA);
    dstClip->addSupportedComponent(ePixelComponentRGB);
    dstClip->addSupportedComponent(ePixelComponentAlpha);
#ifdef OFX_EXTENSIONS_NATRON
    dstClip->addSupportedComponent(ePixelComponentXY);
#endif
    dstClip->setSupportsTiles(kSupportsTiles);

    // make some pages and to things in
    PageParamDescriptor *page = desc.definePageParam("Controls");

    bool hostHasNativeOverlayForPosition;
    // translate
    {
        Double2DParamDescriptor* param = desc.defineDouble2DParam(kParamTranslate);
        param->setLabel(kParamTranslateLabel);
        param->setHint(kParamTranslateHint);
        param->setDoubleType(eDoubleTypeXYAbsolute);
        param->setDefaultCoordinateSystem(eCoordinatesNormalised);
        param->setDefault(0., 0.);
        param->setRange(-DBL_MAX, -DBL_MAX, DBL_MAX, DBL_MAX); // Resolve requires range and display range or values are clamped to (-1,1)
        param->setDisplayRange(-10000, -10000, 10000, 10000); // Resolve requires display range or values are clamped to (-1,1)
        hostHasNativeOverlayForPosition = param->getHostHasNativeOverlayHandle();
        if (hostHasNativeOverlayForPosition) {
            param->setUseHostNativeOverlayHandle(true);
        }
        if (page) {
            page->addChild(*param);
        }
    }
    {
        BooleanParamDescriptor* param = desc.defineBooleanParam(kParamInteractive);
        param->setLabel(kParamInteractiveLabel);
        param->setHint(kParamInteractiveHint);
        param->setAnimates(false);
        if (page) {
            page->addChild(*param);
        }

        //Do not show this parameter if the host handles the interact
        if (hostHasNativeOverlayForPosition) {
            param->setIsSecret(true);
        }
    }
}

OFX::ImageEffect*
PositionPluginFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum /*context*/)
{
    return new PositionPlugin(handle);
}


static PositionPluginFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
mRegisterPluginFactoryInstance(p)

OFXS_NAMESPACE_ANONYMOUS_EXIT
