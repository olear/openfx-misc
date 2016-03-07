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
 * OFX CImgDilate plugin.
 */

#include <memory>
#include <cmath>
#include <cstring>
#include <algorithm>
#ifdef _WINDOWS
#include <windows.h>
#endif

#include "ofxsProcessing.H"
#include "ofxsMaskMix.h"
#include "ofxsMacros.h"
#include "ofxsCoords.h"
#include "ofxsCopier.h"

#include "CImgFilter.h"

using namespace OFX;

OFXS_NAMESPACE_ANONYMOUS_ENTER

#define kPluginName          "DilateCImg"
#define kPluginGrouping      "Filter"
#define kPluginDescription \
"Dilate (or erode) input stream by a rectangular structuring element of specified size and Neumann boundary conditions (pixels out of the image get the value of the nearest pixel).\n" \
"A negative size will perform an erosion instead of a dilation.\n" \
"Different sizes can be given for the x and y axis.\n" \
"Uses the 'dilate' and 'erode' functions from the CImg library.\n" \
"CImg is a free, open-source library distributed under the CeCILL-C " \
"(close to the GNU LGPL) or CeCILL (compatible with the GNU GPL) licenses. " \
"It can be used in commercial applications (see http://cimg.sourceforge.net)."

#define kPluginIdentifier    "net.sf.cimg.CImgDilate"
// History:
// version 1.0: initial version
// version 2.0: use kNatronOfxParamProcess* parameters
#define kPluginVersionMajor 2 // Incrementing this number means that you have broken backwards compatibility of the plug-in.
#define kPluginVersionMinor 0 // Increment this when you have fixed a bug or made it faster.

#define kSupportsComponentRemapping 1
#define kSupportsTiles 1
#define kSupportsMultiResolution 1
#define kSupportsRenderScale 1
#define kSupportsMultipleClipPARs false
#define kSupportsMultipleClipDepths false
#define kRenderThreadSafety eRenderFullySafe
#ifdef cimg_use_openmp
#define kHostFrameThreading false
#else
#define kHostFrameThreading true
#endif
#define kSupportsRGBA true
#define kSupportsRGB true
#define kSupportsXY true
#define kSupportsAlpha true

#define kParamSize "size"
#define kParamSizeLabel "size"
#define kParamSizeHint "Width/height of the rectangular structuring element is 2*size+1, in pixel units (>=0)."
#define kParamSizeDefault 1


/// Dilate plugin
struct CImgDilateParams
{
    int sx;
    int sy;
};

class CImgDilatePlugin : public CImgFilterPluginHelper<CImgDilateParams,false>
{
public:

    CImgDilatePlugin(OfxImageEffectHandle handle)
    : CImgFilterPluginHelper<CImgDilateParams,false>(handle, kSupportsComponentRemapping, kSupportsTiles, kSupportsMultiResolution, kSupportsRenderScale, /*defaultUnpremult=*/true, /*defaultProcessAlphaOnRGBA=*/false)
    {
        _size  = fetchInt2DParam(kParamSize);
        assert(_size);
    }

    virtual void getValuesAtTime(double time, CImgDilateParams& params) OVERRIDE FINAL
    {
        _size->getValueAtTime(time, params.sx, params.sy);
    }

    // compute the roi required to compute rect, given params. This roi is then intersected with the image rod.
    // only called if mix != 0.
    virtual void getRoI(const OfxRectI& rect, const OfxPointD& renderScale, const CImgDilateParams& params, OfxRectI* roi) OVERRIDE FINAL
    {
        int delta_pix_x = (int)std::ceil(std::abs(params.sx) * renderScale.x);
        int delta_pix_y = (int)std::ceil(std::abs(params.sy) * renderScale.y);
        roi->x1 = rect.x1 - delta_pix_x;
        roi->x2 = rect.x2 + delta_pix_x;
        roi->y1 = rect.y1 - delta_pix_y;
        roi->y2 = rect.y2 + delta_pix_y;
    }

    virtual void render(const OFX::RenderArguments &args, const CImgDilateParams& params, int /*x1*/, int /*y1*/, cimg_library::CImg<float>& cimg) OVERRIDE FINAL
    {
        // PROCESSING.
        // This is the only place where the actual processing takes place
        if (params.sx > 0 || params.sy > 0) {
            cimg.dilate((unsigned int)std::floor(std::max(0, params.sx) * args.renderScale.x) * 2 + 1,
                        (unsigned int)std::floor(std::max(0, params.sy) * args.renderScale.y) * 2 + 1);
        }
        if (params.sx < 0 || params.sy < 0) {
            cimg.erode((unsigned int)std::floor(std::max(0, -params.sx) * args.renderScale.x) * 2 + 1,
                       (unsigned int)std::floor(std::max(0, -params.sy) * args.renderScale.y) * 2 + 1);
        }
    }

    virtual bool isIdentity(const OFX::IsIdentityArguments &args, const CImgDilateParams& params) OVERRIDE FINAL
    {
        return (std::floor(params.sx * args.renderScale.x) == 0 && std::floor(params.sy * args.renderScale.y) == 0);
    };

private:

    // params
    OFX::Int2DParam *_size;
};


mDeclarePluginFactory(CImgDilatePluginFactory, {}, {});

void CImgDilatePluginFactory::describe(OFX::ImageEffectDescriptor& desc)
{
    // basic labels
    desc.setLabel(kPluginName);
    desc.setPluginGrouping(kPluginGrouping);
    desc.setPluginDescription(kPluginDescription);

    // add supported context
    desc.addSupportedContext(eContextFilter);
    desc.addSupportedContext(eContextGeneral);

    // add supported pixel depths
    //desc.addSupportedBitDepth(eBitDepthUByte);
    //desc.addSupportedBitDepth(eBitDepthUShort);
    desc.addSupportedBitDepth(eBitDepthFloat);

    // set a few flags
    desc.setSingleInstance(false);
    desc.setHostFrameThreading(kHostFrameThreading);
    desc.setSupportsMultiResolution(kSupportsMultiResolution);
    desc.setSupportsTiles(kSupportsTiles);
    desc.setTemporalClipAccess(false);
    desc.setRenderTwiceAlways(true);
    desc.setSupportsMultipleClipPARs(kSupportsMultipleClipPARs);
    desc.setSupportsMultipleClipDepths(kSupportsMultipleClipDepths);
    desc.setRenderThreadSafety(kRenderThreadSafety);
}

void CImgDilatePluginFactory::describeInContext(OFX::ImageEffectDescriptor& desc, OFX::ContextEnum context)
{
    // create the clips and params
    OFX::PageParamDescriptor *page = CImgDilatePlugin::describeInContextBegin(desc, context,
                                                                              kSupportsRGBA,
                                                                              kSupportsRGB,
                                                                              kSupportsXY,
                                                                              kSupportsAlpha,
                                                                              kSupportsTiles,
                                                                              /*processRGB=*/true,
                                                                              /*processAlpha*/false,
                                                                              /*processIsSecret=*/false);

    {
        OFX::Int2DParamDescriptor *param = desc.defineInt2DParam(kParamSize);
        param->setLabel(kParamSizeLabel);
        param->setHint(kParamSizeHint);
        param->setRange(-1000, -1000, 1000, 1000);
        param->setDisplayRange(-100, -100, 100, 100);
        param->setDefault(kParamSizeDefault, kParamSizeDefault);
        if (page) {
            page->addChild(*param);
        }
    }

    CImgDilatePlugin::describeInContextEnd(desc, context, page);
}

OFX::ImageEffect* CImgDilatePluginFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum /*context*/)
{
    return new CImgDilatePlugin(handle);
}


static CImgDilatePluginFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
mRegisterPluginFactoryInstance(p)

OFXS_NAMESPACE_ANONYMOUS_EXIT
