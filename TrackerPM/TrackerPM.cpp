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
 * Basic tracker with exhaustive search algorithm OFX plugin.
 */

#include <cmath>
#include <map>
#include <limits>
#include <algorithm>

#include "ofxsProcessing.H"
#include "ofxsTracking.h"
#include "ofxsCoords.h"

using namespace OFX;

OFXS_NAMESPACE_ANONYMOUS_ENTER

#define kPluginName "TrackerPM"
#define kPluginGrouping "Transform"
#define kPluginDescription \
"Point tracker based on pattern matching using an exhaustive search within an image region.\n" \
"The Mask input is used to weight the pattern, so that only pixels from the Mask will be tracked. \n" \
"The tracker always takes the previous/next frame as reference when searching for a pattern in an image. This can " \
"overtime make a track drift from its original pattern.\n"\
"Canceling a tracking operation will not wipe all the data analysed so far. If you resume a previously canceled tracking, " \
"the tracker will continue tracking, picking up the previous/next frame as reference. "
#define kPluginIdentifier "net.sf.openfx.TrackerPM"
#define kPluginVersionMajor 1 // Incrementing this number means that you have broken backwards compatibility of the plug-in.
#define kPluginVersionMinor 0 // Increment this when you have fixed a bug or made it faster.

#define kSupportsTiles 1
#define kSupportsMultiResolution 1
#define kSupportsRenderScale 1
#define kSupportsMultipleClipPARs false
#define kSupportsMultipleClipDepths false
#define kRenderThreadSafety eRenderFullySafe

#define kParamScore "score"
#define kParamScoreLabel "Score"
#define kParamScoreHint "Correlation score computation method"
#define kParamScoreOptionSSD "SSD"
#define kParamScoreOptionSSDHint "Sum of Squared Differences"
#define kParamScoreOptionSAD "SAD"
#define kParamScoreOptionSADHint "Sum of Absolute Differences, more robust to occlusions"
#define kParamScoreOptionNCC "NCC"
#define kParamScoreOptionNCCHint "Normalized Cross-Correlation"
#define kParamScoreOptionZNCC "ZNCC"
#define kParamScoreOptionZNCCHint "Zero-mean Normalized Cross-Correlation, less sensitive to illumination changes"


enum TrackerScoreEnum
{
    eTrackerSSD = 0,
    eTrackerSAD,
    eTrackerNCC,
    eTrackerZNCC
};

class TrackerPMProcessorBase;
////////////////////////////////////////////////////////////////////////////////
/** @brief The plugin that does our work */
class TrackerPMPlugin : public GenericTrackerPlugin
{
public:
    /** @brief ctor */
    TrackerPMPlugin(OfxImageEffectHandle handle)
    : GenericTrackerPlugin(handle)
    , _score(0)
    , _center(0)
    , _offset(0)
    , _innerBtmLeft(0)
    , _innerTopRight(0)
    , _outerBtmLeft(0)
    , _outerTopRight(0)
    {
        _maskClip = fetchClip(getContext() == OFX::eContextPaint ? "Brush" : "Mask");
        assert(!_maskClip || _maskClip->getPixelComponents() == ePixelComponentAlpha);
        _score = fetchChoiceParam(kParamScore);
        assert(_score);
        
        _center = fetchDouble2DParam(kParamTrackingCenterPoint);
        _offset = fetchDouble2DParam(kParamTrackingOffset);
        _innerBtmLeft = fetchDouble2DParam(kParamTrackingPatternBoxBtmLeft);
        _innerTopRight = fetchDouble2DParam(kParamTrackingPatternBoxTopRight);
        _outerBtmLeft = fetchDouble2DParam(kParamTrackingSearchBoxBtmLeft);
        _outerTopRight = fetchDouble2DParam(kParamTrackingSearchBoxTopRight);
        assert(_center && _offset && _innerTopRight && _innerBtmLeft && _outerTopRight && _outerBtmLeft);
    }
    
    
private:
    /**
     * @brief Override to track the entire range between [first,last].
     * @param forward If true then it should track from first to last, otherwise it should track
     * from last to first.
     * @param currentTime The current time at which the track has been requested.
     **/
    virtual void trackRange(const OFX::TrackArguments& args);
    
    template <int nComponents>
    void trackInternal(OfxTime refTime, OfxTime otherTime, const OFX::TrackArguments& args);

    template <class PIX, int nComponents, int maxValue>
    void trackInternalForDepth(OfxTime refTime,
                               const OfxRectD& refBounds,
                               const OfxPointD& refCenter,
                               const OfxPointD& refCenterWithOffset,
                               const OFX::Image* refImg,
                               const OFX::Image* maskImg,
                               OfxTime otherTime,
                               const OfxRectD& trackSearchBounds,
                               const OFX::Image* otherImg);

    /* set up and run a processor */
    void setupAndProcess(TrackerPMProcessorBase &processor,
                         OfxTime refTime,
                         const OfxRectD& refBounds,
                         const OfxPointD& refCenter,
                         const OfxPointD& refCenterWithOffset,
                         const OFX::Image* refImg,
                         const OFX::Image* maskImg,
                         OfxTime otherTime,
                         const OfxRectD& trackSearchBounds,
                         const OFX::Image* otherImg);

    OFX::Clip *_maskClip;
    ChoiceParam* _score;
    
    OFX::Double2DParam* _center;
    OFX::Double2DParam* _offset;
    OFX::Double2DParam* _innerBtmLeft;
    OFX::Double2DParam* _innerTopRight;
    OFX::Double2DParam* _outerBtmLeft;
    OFX::Double2DParam* _outerTopRight;
};


class TrackerPMProcessorBase : public OFX::ImageProcessor
{
protected:
    const OFX::Image *_otherImg;
    OfxRectI _refRectPixel;
    OfxPointI _refCenterI;
    std::pair<OfxPointD,double> _bestMatch; //< the results for the current processor
    OFX::MultiThread::Mutex _bestMatchMutex; //< this is used so we can multi-thread the tracking and protect the shared results
    
public:
    TrackerPMProcessorBase(OFX::ImageEffect &instance)
    : OFX::ImageProcessor(instance)
    , _otherImg(0)
    , _refRectPixel()
    , _refCenterI()
    {
        _bestMatch.second = std::numeric_limits<double>::infinity();

    }

    virtual ~TrackerPMProcessorBase()
    {
    }

    /** @brief set the processing parameters. return false if processing cannot be done. */
    virtual bool setValues(const OFX::Image *ref, const OFX::Image *other, const OFX::Image *mask,
                           const OfxRectI& pattern, const OfxPointI& centeri) = 0;

    /**
     * @brief Retrieves the results of the track. Must be called once process() returns so it is thread safe.
     **/
    const OfxPointD& getBestMatch() const { return _bestMatch.first; }
    double getBestScore() const { return _bestMatch.second; }
};


// The "masked", "filter" and "clamp" template parameters allow filter-specific optimization
// by the compiler, using the same generic code for all filters.
template <class PIX, int nComponents, int maxValue, TrackerScoreEnum scoreType>
class TrackerPMProcessor : public TrackerPMProcessorBase
{
protected:
    std::auto_ptr<OFX::ImageMemory> _patternImg;
    PIX *_patternData;
    std::auto_ptr<OFX::ImageMemory> _weightImg;
    float *_weightData;
    double _weightTotal;
public:
    TrackerPMProcessor(OFX::ImageEffect &instance)
    : TrackerPMProcessorBase(instance)
    , _patternImg(0)
    , _patternData(0)
    , _weightImg(0)
    , _weightData(0)
    , _weightTotal(0.)
    {
    }

    ~TrackerPMProcessor()
    {
    }
private:
    /** @brief set the processing parameters. return false if processing cannot be done. */
    virtual bool setValues(const OFX::Image *ref, const OFX::Image *other, const OFX::Image *mask,
                           const OfxRectI& pattern, const OfxPointI& centeri)
    {
        size_t rowsize = pattern.x2 - pattern.x1;
        size_t nPix = rowsize * (pattern.y2 - pattern.y1);
        
        
        // This happens if the pattern is empty. Most probably this is because it is totally outside the image
        // we better return quickly.
        if (nPix == 0) {
            return false;
        }
        
        _patternImg.reset(new ImageMemory(sizeof(PIX) * nComponents * nPix, &_effect));
        _weightImg.reset(new ImageMemory(sizeof(float) * nPix, &_effect));
        _otherImg = other;
        _refRectPixel = pattern;
        _refCenterI = centeri;

        _patternData = (PIX*)_patternImg->lock();
        _weightData = (float*)_weightImg->lock();

        // sliding pointers
        long patternIdx = 0; // sliding index
        PIX *patternPtr = _patternData;
        float *weightPtr = _weightData;
        _weightTotal = 0.;

        // extract ref and mask
        for (int i = _refRectPixel.y1; i < _refRectPixel.y2; ++i) {
            for (int j = _refRectPixel.x1; j < _refRectPixel.x2; ++j, ++weightPtr, patternPtr += nComponents, ++patternIdx) {
                assert(patternIdx == ((i - _refRectPixel.y1) * (_refRectPixel.x2 - _refRectPixel.x1) + (j - _refRectPixel.x1)));
                PIX *refPix = (PIX*) ref->getPixelAddress(_refCenterI.x + j, _refCenterI.y + i);

                if (!refPix) {
                    // no reference pixel, set weight to 0
                    *weightPtr = 0.f;
                    for (int c = 0; c < nComponents; ++c) {
                        patternPtr[c] = PIX();
                    }
                } else {
                    if (!mask) {
                        // no mask, weight is uniform
                        *weightPtr = 1.f;
                    } else {
                        PIX *maskPix = (PIX*) mask->getPixelAddress(_refCenterI.x + j, _refCenterI.y + i);
                        // weight is zero if there's a mask but we're outside of it
                        *weightPtr = maskPix ? (*maskPix/(float)maxValue) : 0.f;
                    }
                    for (int c = 0; c < nComponents; ++c) {
                        patternPtr[c] = refPix[c];
                    }
                }
                _weightTotal += *weightPtr;
            }
        }
        return (_weightTotal > 0);
    }

    void multiThreadProcessImages(OfxRectI procWindow) {
        switch (scoreType) {
            case eTrackerSSD:
                return multiThreadProcessImagesForScore<eTrackerSSD>(procWindow);
            case eTrackerSAD:
                return multiThreadProcessImagesForScore<eTrackerSAD>(procWindow);
            case eTrackerNCC:
                return multiThreadProcessImagesForScore<eTrackerNCC>(procWindow);
            case eTrackerZNCC:
                return multiThreadProcessImagesForScore<eTrackerZNCC>(procWindow);
        }
    }

    template<enum TrackerScoreEnum scoreTypeE>
    double computeScore(int x, int y, const double refMean[3])
    {
        double score = 0;
        double otherSsq = 0.;
        double otherMean[3];
        const int scoreComps = std::min(nComponents, 3);
        if (scoreTypeE == eTrackerZNCC) {
            for (int c = 0; c < 3; ++c) {
                otherMean[c] = 0;
            }
            // sliding pointers
            long patternIdx = 0; // sliding index
            const PIX *patternPtr = _patternData;
            float *weightPtr = _weightData;
            for (int i = _refRectPixel.y1; i < _refRectPixel.y2; ++i) {
                for (int j = _refRectPixel.x1; j < _refRectPixel.x2; ++j, ++weightPtr, patternPtr += nComponents, ++patternIdx) {
                    assert(patternIdx == ((i - _refRectPixel.y1) * (_refRectPixel.x2 - _refRectPixel.x1) + (j - _refRectPixel.x1)));
                   // take nearest pixel in other image (more chance to get a track than with black)
                    int otherx = x + j;
                    int othery = y + i;
                    otherx = std::max(_otherImg->getBounds().x1,std::min(otherx,_otherImg->getBounds().x2-1));
                    othery = std::max(_otherImg->getBounds().y1,std::min(othery,_otherImg->getBounds().y2-1));
                    const PIX *otherPix = (const PIX *) _otherImg->getPixelAddress(otherx, othery);
                    for (int c = 0; c < scoreComps; ++c) {
                        otherMean[c] += *weightPtr * otherPix[c];
                    }
                }
            }
            for (int c = 0; c < scoreComps; ++c) {
                otherMean[c] /= _weightTotal;
            }
        }

        // sliding pointers
        long patternIdx = 0; // sliding index
        const PIX *patternPtr = _patternData;
        float *weightPtr = _weightData;

        for (int i = _refRectPixel.y1; i < _refRectPixel.y2; ++i) {
            for (int j = _refRectPixel.x1; j < _refRectPixel.x2; ++j, ++weightPtr, patternPtr += nComponents, ++patternIdx) {
                assert(patternIdx == ((i - _refRectPixel.y1) * (_refRectPixel.x2 - _refRectPixel.x1) + (j - _refRectPixel.x1)));
                const PIX * const refPix = patternPtr;

                const float weight = *weightPtr;

                // take nearest pixel in other image (more chance to get a track than with black)
                int otherx = x + j;
                int othery = y + i;
                otherx = std::max(_otherImg->getBounds().x1,std::min(otherx,_otherImg->getBounds().x2-1));
                othery = std::max(_otherImg->getBounds().y1,std::min(othery,_otherImg->getBounds().y2-1));
                const PIX *otherPix = (const PIX *) _otherImg->getPixelAddress(otherx, othery);

                ///the search window & pattern window have been intersected to the reference image's bounds
                assert(refPix && otherPix);
                for (int c = 0; c < scoreComps; ++c) {
                    switch (scoreTypeE) {
                        case eTrackerSSD:
                            // reference is squared in SSD, so is the weight
                            score += weight * weight * aggregateSD(refPix[c], otherPix[c]);
                            break;
                        case eTrackerSAD:
                            score += weight * aggregateAD(refPix[c], otherPix[c]);
                            break;
                        case eTrackerNCC:
                            score += weight * aggregateCC(refPix[c], otherPix[c]);
                            otherSsq -= weight * aggregateCC(otherPix[c], otherPix[c]);
                            break;
                        case eTrackerZNCC:
                            score += weight * aggregateNCC(refPix[c], refMean[c], otherPix[c], otherMean[c]);
                            otherSsq -= weight * aggregateNCC(otherPix[c], otherMean[c], otherPix[c], otherMean[c]);
                            break;
                    }
                }
            }
        }
        if (scoreTypeE == eTrackerNCC || scoreTypeE == eTrackerZNCC) {
            double sdev = std::sqrt(otherSsq);
            if (sdev != 0.) {
                score /= sdev;
            } else {
                score = std::numeric_limits<double>::infinity();
            }
        }
        return score;
    }

    template<enum TrackerScoreEnum scoreTypeE>
    void multiThreadProcessImagesForScore(const OfxRectI& procWindow)
    {
        assert(_patternImg.get() && _patternData && _weightImg.get() && _weightData && _otherImg && _weightTotal > 0.);
        assert(scoreType == scoreTypeE);
        double bestScore = std::numeric_limits<double>::infinity();
        OfxPointI point;
        point.x = -1;
        point.y = -1;

        ///For every pixel in the sub window of the search area we find the pixel
        ///that minimize the sum of squared differences between the pattern in the ref image
        ///and the pattern in the other image.

        const int scoreComps = std::min(nComponents,3);
        double refMean[3];
        if (scoreTypeE == eTrackerZNCC) {
            for (int c = 0; c < 3; ++c) {
                refMean[c] = 0;
            }
        }
        if (scoreTypeE == eTrackerZNCC) {
            // sliding pointers
            long patternIdx = 0; // sliding index
            const PIX *patternPtr = _patternData;
            float *weightPtr = _weightData;
            for (int i = _refRectPixel.y1; i < _refRectPixel.y2; ++i) {
                for (int j = _refRectPixel.x1; j < _refRectPixel.x2; ++j, ++weightPtr, patternPtr += nComponents, ++patternIdx) {
                    assert(patternIdx == ((i - _refRectPixel.y1) * (_refRectPixel.x2 - _refRectPixel.x1) + (j - _refRectPixel.x1)));
                    const PIX *refPix = patternPtr;
                    for (int c = 0; c < scoreComps; ++c) {
                        refMean[c] += *weightPtr * refPix[c];
                    }
                }
            }
            for (int c = 0; c < scoreComps; ++c) {
                refMean[c] /= _weightTotal;
            }
        }

        ///we're not interested in the alpha channel for RGBA images
        for (int y = procWindow.y1; y < procWindow.y2; ++y) {
            if (_effect.abort()) {
                break;
            }
            
            for (int x = procWindow.x1; x < procWindow.x2; ++x) {
                double score = computeScore<scoreTypeE>(x, y, refMean);
                if (score < bestScore) {
                    bestScore = score;
                    point.x = x;
                    point.y = y;
                }
            }
        }
        
        // do the subpixel refinement, only if the score is a possible winner
        // TODO: only do this for the best match
        double dx = 0.;
        double dy = 0.;

        _bestMatchMutex.lock();
        if (_bestMatch.second < bestScore) {
            _bestMatchMutex.unlock();
        } else {
            // don't block other threads
            _bestMatchMutex.unlock();
            // compute subpixel position.
            double scorepc = computeScore<scoreTypeE>(point.x - 1, point.y, refMean);
            double scorenc = computeScore<scoreTypeE>(point.x + 1, point.y, refMean);
            if (bestScore < scorepc && bestScore <= scorenc) {
                // don't simplify the denominator in the following expression,
                // 2*bestScore - scorenc - scorepc may cause an underflow.
                double factor = 1./((bestScore - scorenc) + (bestScore - scorepc));
                if (factor != 0.) {
                    dx = 0.5 * (scorenc - scorepc) * factor;
                    assert(-0.5 < dx && dx <= 0.5);
                }
            }
            double scorecp = computeScore<scoreTypeE>(point.x, point.y - 1, refMean);
            double scorecn = computeScore<scoreTypeE>(point.x, point.y + 1, refMean);
            if (bestScore < scorecp && bestScore <= scorecn) {
                // don't simplify the denominator in the following expression,
                // 2*bestScore - scorenc - scorepc may cause an underflow.
                double factor = 1./((bestScore - scorecn) + (bestScore - scorecp));
                if (factor != 0.) {
                    dy = 0.5 * (scorecn - scorecp) /((bestScore - scorecn) + (bestScore - scorecp));
                    assert(-0.5 < dy && dy <= 0.5);
                }
            }
            // check again...
            {
                OFX::MultiThread::AutoMutex lock(_bestMatchMutex);
                if (_bestMatch.second > bestScore) {
                    _bestMatch.second = bestScore;
                    _bestMatch.first.x = point.x + dx;
                    _bestMatch.first.y = point.y + dy;
                }
            }

        }
    }

    double aggregateSD(PIX refPix, PIX otherPix)
    {
        double d = (double)refPix - otherPix;
        return d * d;
    }

    double aggregateAD(PIX refPix, PIX otherPix)
    {
        return std::abs((double)refPix - otherPix);
    }

    double aggregateCC(PIX refPix, PIX otherPix)
    {
        return - (double)refPix * otherPix;
    }

    double aggregateNCC(PIX refPix, double refMean, PIX otherPix, double otherMean)
    {
        return - (refPix - refMean) * (otherPix - otherMean);
    }
};


void
TrackerPMPlugin::trackRange(const OFX::TrackArguments& args)
{
    if (!_srcClip) {
        return;
    }
# ifdef kOfxImageEffectPropInAnalysis // removed from OFX 1.4
    // Although the following property has been there since OFX 1.0,
    // it's not in the HostSupport library.
    getPropertySet().propSetInt(kOfxImageEffectPropInAnalysis, 1, false);
#  endif
    //double t1, t2;
    // get the first and last times available on the effect's timeline
    //timeLineGetBounds(t1, t2);

    OfxTime t = args.first;
    bool changeTime = (args.reason == eChangeUserEdit && t == timeLineGetTime());
    std::string name;
    _instanceName->getValueAtTime(t, name);
    assert((args.forward && args.last >= args.first) || (!args.forward && args.last <= args.first));
    bool showProgress = std::abs(args.last - args.first) > 1;
    if (showProgress) {
        progressStart(name);
    }

    while (args.forward ? (t <= args.last) : (t >= args.last)) {
        OfxTime other = args.forward ? (t + 1) : (t - 1);
        
        OFX::PixelComponentEnum srcComponents  = _srcClip->getPixelComponents();
        assert(srcComponents == OFX::ePixelComponentRGB || srcComponents == OFX::ePixelComponentRGBA ||
               srcComponents == OFX::ePixelComponentAlpha);
        
        if (srcComponents == OFX::ePixelComponentRGBA) {
            trackInternal<4>(t, other, args);
        } else if (srcComponents == OFX::ePixelComponentRGB) {
            trackInternal<3>(t, other, args);
        } else {
            assert(srcComponents == OFX::ePixelComponentAlpha);
            trackInternal<1>(t, other, args);
        }
        if (args.forward) {
            ++t;
        } else {
            --t;
        }
        if (changeTime ) {
            // set the timeline to a specific time
            timeLineGotoTime(t);
        }
        if (showProgress && !progressUpdate((t - args.first) / (args.last - args.first))) {
            progressEnd();
            return;
        }
    }
    if (showProgress) {
        progressEnd();
    }
# ifdef kOfxImageEffectPropInAnalysis // removed from OFX 1.4
    getPropertySet().propSetInt(kOfxImageEffectPropInAnalysis, 0, false);
# endif
}

////////////////////////////////////////////////////////////////////////////////
/** @brief render for the filter */

////////////////////////////////////////////////////////////////////////////////
// basic plugin render function, just a skelington to instantiate templates from


static void
getRefBounds(const OfxRectD& refRect, const OfxPointD &refCenter, OfxRectD *bounds)
{
    bounds->x1 = refCenter.x + refRect.x1;
    bounds->x2 = refCenter.x + refRect.x2;
    bounds->y1 = refCenter.y + refRect.y1;
    bounds->y2 = refCenter.y + refRect.y2;

    // make the window at least 2 pixels high/wide
    // (this should never happen, of course)
    if (bounds->x2 < bounds->x1 + 2) {
        bounds->x1 = (bounds->x1 + bounds->x2) / 2 - 1;
        bounds->x2 = bounds->x1 + 2;
    }
    if (bounds->y2 < bounds->y1 + 2) {
        bounds->y1 = (bounds->y1 + bounds->y2) / 2 - 1;
        bounds->y2 = bounds->y1 + 2;
    }
}

static void
getTrackSearchBounds(const OfxRectD& refRect, const OfxPointD &refCenter, const OfxRectD& searchRect, OfxRectD *bounds)
{
    // subtract the pattern window so that we don't check for pixels out of the search window
    bounds->x1 = refCenter.x + searchRect.x1 - refRect.x1;
    bounds->y1 = refCenter.y + searchRect.y1 - refRect.y1;
    bounds->x2 = refCenter.x + searchRect.x2 - refRect.x2;
    bounds->y2 = refCenter.y + searchRect.y2 - refRect.y2;

    // if the window is empty, make it at least 1 pixel high/wide
    if (bounds->x2 <= bounds->x1) {
        bounds->x1 = (bounds->x1 + bounds->x2) / 2;
        bounds->x2 = bounds->x1 + 1;
    }
    if (bounds->y2 <= bounds->y1) {
        bounds->y1 = (bounds->y1 + bounds->y2) / 2;
        bounds->y2 = bounds->y1 + 1;
    }
}

static void
getOtherBounds(const OfxPointD &refCenter, const OfxRectD& searchRect, OfxRectD *bounds)
{
    // subtract the pattern window so that we don't check for pixels out of the search window
    bounds->x1 = refCenter.x + searchRect.x1;
    bounds->y1 = refCenter.y + searchRect.y1;
    bounds->x2 = refCenter.x + searchRect.x2;
    bounds->y2 = refCenter.y + searchRect.y2;

    // if the window is empty, make it at least 1 pixel high/wide
    if (bounds->x2 <= bounds->x1) {
        bounds->x1 = (bounds->x1 + bounds->x2) / 2;
        bounds->x2 = bounds->x1 + 1;
    }
    if (bounds->y2 <= bounds->y1) {
        bounds->y1 = (bounds->y1 + bounds->y2) / 2;
        bounds->y2 = bounds->y1 + 1;
    }
}

/* set up and run a processor */
void
TrackerPMPlugin::setupAndProcess(TrackerPMProcessorBase &processor,
                                 OfxTime refTime,
                                 const OfxRectD& refBounds,
                                 const OfxPointD& refCenter,
                                 const OfxPointD& refCenterWithOffset,
                                 const OFX::Image* refImg,
                                 const OFX::Image* maskImg,
                                 OfxTime otherTime,
                                 const OfxRectD& trackSearchBounds,
                                 const OFX::Image* otherImg)
{
    if (!_srcClip) {
        return;
    }
    const double par = _srcClip->getPixelAspectRatio();
    const OfxPointD rsOne = {1., 1.};
    OfxRectI trackSearchBoundsPixel;
    OFX::Coords::toPixelEnclosing(trackSearchBounds, rsOne, par, &trackSearchBoundsPixel);

    // compute the pattern window in pixel coords
    OfxRectI refRectPixel;
    OFX::Coords::toPixelEnclosing(refBounds, rsOne, par, &refRectPixel);

    // round center to nearest pixel center
    OfxPointI refCenterI;
    OfxPointD refCenterPixelSub;
    OFX::Coords::toPixel(refCenterWithOffset, rsOne, par, &refCenterI);
    OFX::Coords::toPixelSub(refCenterWithOffset, rsOne, par, &refCenterPixelSub);

    //Clip the refRectPixel to the bounds of the ref image
    bool intersect = OFX::Coords::rectIntersection(refRectPixel, refImg->getBounds(), &refRectPixel);
    
    if (!intersect) {
        // can't track: erase any existing track
        _center->deleteKeyAtTime(otherTime);
    }
    refRectPixel.x1 -= refCenterI.x;
    refRectPixel.x2 -= refCenterI.x;
    refRectPixel.y1 -= refCenterI.y;
    refRectPixel.y2 -= refCenterI.y;

    // set the render window
    processor.setRenderWindow(trackSearchBoundsPixel);
    
    bool canProcess = processor.setValues(refImg, otherImg, maskImg, refRectPixel, refCenterI);
    
    if (!canProcess) {
        // can't track: erase any existing track
        _center->deleteKeyAtTime(otherTime);
    } else {
        // Call the base class process member, this will call the derived templated process code
        processor.process();

        //////////////////////////////////
        // TODO: subpixel interpolation //
        //////////////////////////////////

        ///ok the score is now computed, update the center
        if (processor.getBestScore() == std::numeric_limits<double>::infinity()) {
            // can't track: erase any existing track
            _center->deleteKeyAtTime(otherTime);
        } else {
            
            // Offset the newCenter by the offset a thaat time
            OfxPointD otherOffset;
            _offset->getValueAtTime(otherTime, otherOffset.x, otherOffset.y);
            
            OfxPointD newCenterPixelSub;
            OfxPointD newCenter;
            const OfxPointD& bestMatch = processor.getBestMatch();

            newCenterPixelSub.x = refCenterPixelSub.x + bestMatch.x - refCenterI.x;
            newCenterPixelSub.y = refCenterPixelSub.y + bestMatch.y - refCenterI.y;
            OFX::Coords::toCanonicalSub(newCenterPixelSub, rsOne, par, &newCenter);
            
            //Commented-out for Natron compat: Natron does beginEditBlock in the main-thread, hence
            //since the instanceChanged action is executed in multiple separated thread by Natron when tracking, there's no
            //telling that the actual setting of the value will be done when the next frame is tracked
            //beginEditBlock("trackerUpdate");
            // create a keyframe at starting point
            _center->setValueAtTime(refTime, refCenter.x, refCenter.y);
            // create a keyframe at end point
            _center->setValueAtTime(otherTime, newCenter.x - otherOffset.x, newCenter.y - otherOffset.y);
           // endEditBlock();
        }
    }
}

template <class PIX, int nComponents, int maxValue>
void
TrackerPMPlugin::trackInternalForDepth(OfxTime refTime,
                                       const OfxRectD& refBounds,
                                       const OfxPointD& refCenter,
                                       const OfxPointD& refCenterWithOffset,
                                       const OFX::Image* refImg,
                                       const OFX::Image* maskImg,
                                       OfxTime otherTime,
                                       const OfxRectD& trackSearchBounds,
                                       const OFX::Image* otherImg)
{
    TrackerScoreEnum typeE = (TrackerScoreEnum)_score->getValueAtTime(refTime);

    switch (typeE) {
        case eTrackerSSD: {
            TrackerPMProcessor<PIX, nComponents, maxValue, eTrackerSSD> fred(*this);
            setupAndProcess(fred, refTime, refBounds, refCenter, refCenterWithOffset, refImg, maskImg, otherTime, trackSearchBounds, otherImg);
        }   break;
        case eTrackerSAD: {
            TrackerPMProcessor<PIX, nComponents, maxValue, eTrackerSAD> fred(*this);
            setupAndProcess(fred, refTime, refBounds, refCenter, refCenterWithOffset, refImg, maskImg, otherTime, trackSearchBounds, otherImg);
        }   break;
        case eTrackerNCC: {
            TrackerPMProcessor<PIX, nComponents, maxValue, eTrackerNCC> fred(*this);
            setupAndProcess(fred, refTime, refBounds, refCenter, refCenterWithOffset,  refImg, maskImg, otherTime, trackSearchBounds, otherImg);
        }   break;
        case eTrackerZNCC: {
            TrackerPMProcessor<PIX, nComponents, maxValue, eTrackerZNCC> fred(*this);
            setupAndProcess(fred, refTime, refBounds, refCenter, refCenterWithOffset, refImg, maskImg, otherTime, trackSearchBounds, otherImg);
        }   break;
    }
}


// the internal render function
template <int nComponents>
void
TrackerPMPlugin::trackInternal(OfxTime refTime, OfxTime otherTime, const OFX::TrackArguments& args)
{
    OfxRectD refRect;
    _innerBtmLeft->getValueAtTime(refTime, refRect.x1, refRect.y1);
    _innerTopRight->getValueAtTime(refTime, refRect.x2, refRect.y2);
    OfxPointD refCenter;
    _center->getValueAtTime(refTime, refCenter.x, refCenter.y);
    OfxRectD searchRect;
    _outerBtmLeft->getValueAtTime(refTime, searchRect.x1, searchRect.y1);
    _outerTopRight->getValueAtTime(refTime, searchRect.x2, searchRect.y2);

    OfxPointD offset;
    _offset->getValueAtTime(refTime, offset.x, offset.y);
    
    OfxPointD refCenterWithOffset;
    refCenterWithOffset.x = refCenter.x + offset.x;
    refCenterWithOffset.y = refCenter.y + offset.y;
    
    OfxRectD refBounds;
    getRefBounds(refRect, refCenterWithOffset, &refBounds);

    OfxRectD otherBounds;
    getOtherBounds(refCenterWithOffset, searchRect, &otherBounds);

    std::auto_ptr<const OFX::Image> srcRef((_srcClip && _srcClip->isConnected()) ?
                                           _srcClip->fetchImage(refTime, refBounds) : 0);
    std::auto_ptr<const OFX::Image> srcOther((_srcClip && _srcClip->isConnected()) ?
                                             _srcClip->fetchImage(otherTime, otherBounds) : 0);
    if (!srcRef.get() || !srcOther.get()) {
        return;
    }
    if (srcRef.get()) {
        if (srcRef->getRenderScale().x != args.renderScale.x ||
            srcRef->getRenderScale().y != args.renderScale.y) {
            setPersistentMessage(OFX::Message::eMessageError, "", "OFX Host gave image with wrong scale or field properties");
            OFX::throwSuiteStatusException(kOfxStatFailed);
        }
    }
    if (srcOther.get()) {
        if (srcOther->getRenderScale().x != args.renderScale.x ||
            srcOther->getRenderScale().y != args.renderScale.y) {
            setPersistentMessage(OFX::Message::eMessageError, "", "OFX Host gave image with wrong scale or field properties");
            OFX::throwSuiteStatusException(kOfxStatFailed);
        }
    }
    // renderScale should never be something else than 1 when called from ActionInstanceChanged
    if ((srcRef->getPixelDepth() != srcOther->getPixelDepth()) ||
        (srcRef->getPixelComponents() != srcOther->getPixelComponents()) ||
        srcRef->getRenderScale().x != 1. || srcRef->getRenderScale().y != 1 ||
        srcOther->getRenderScale().x != 1. || srcOther->getRenderScale().y != 1) {
        OFX::throwSuiteStatusException(kOfxStatErrImageFormat);
    }

    OFX::BitDepthEnum srcBitDepth = srcRef->getPixelDepth();

    //  mask cannot be black and transparent, so an empty mask means mask is disabled.
    // auto ptr for the mask.
    std::auto_ptr<const OFX::Image> mask((_maskClip && _maskClip->isConnected()) ?
                                         _maskClip->fetchImage(refTime) : 0);
    if (mask.get()) {
        if (mask->getRenderScale().x != args.renderScale.x ||
            mask->getRenderScale().y != args.renderScale.y) {
            setPersistentMessage(OFX::Message::eMessageError, "", "OFX Host gave image with wrong scale or field properties");
            OFX::throwSuiteStatusException(kOfxStatFailed);
        }
    }

    OfxRectD trackSearchBounds;
    getTrackSearchBounds(refRect, refCenterWithOffset, searchRect, &trackSearchBounds);

    switch (srcBitDepth) {
        case OFX::eBitDepthUByte: {
            trackInternalForDepth<unsigned char, nComponents, 255>(refTime, refBounds, refCenter, refCenterWithOffset, srcRef.get(), mask.get(), otherTime, trackSearchBounds, srcOther.get());
        }   break;
        case OFX::eBitDepthUShort: {
            trackInternalForDepth<unsigned short, nComponents, 65535>(refTime, refBounds, refCenter, refCenterWithOffset, srcRef.get(), mask.get(), otherTime, trackSearchBounds, srcOther.get());
        }   break;
        case OFX::eBitDepthFloat: {
            trackInternalForDepth<float, nComponents, 1>(refTime, refBounds, refCenter, refCenterWithOffset, srcRef.get(), mask.get(), otherTime, trackSearchBounds, srcOther.get());
        }   break;
        default:
            OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
}


mDeclarePluginFactory(TrackerPMPluginFactory, {}, {});

void TrackerPMPluginFactory::describe(OFX::ImageEffectDescriptor &desc)
{
    // basic labels
    desc.setLabel(kPluginName);
    desc.setPluginGrouping(kPluginGrouping);
    desc.setPluginDescription(kPluginDescription);

    // description common to all trackers
    genericTrackerDescribe(desc);

    // add the additional supported contexts
    desc.addSupportedContext(eContextPaint); // this tracker can be masked

    // supported bit depths depend on the tracking algorithm.
    desc.addSupportedBitDepth(eBitDepthUByte);
    desc.addSupportedBitDepth(eBitDepthUShort);
    desc.addSupportedBitDepth(eBitDepthFloat);

    // single instance depends on the algorithm
    desc.setSingleInstance(false);

    // rendertwicealways must be set to true if the tracker cannot handle interlaced content (most don't)
    desc.setRenderTwiceAlways(true);
    desc.setRenderThreadSafety(kRenderThreadSafety);
    desc.setOverlayInteractDescriptor(new TrackerRegionOverlayDescriptor);
}

void TrackerPMPluginFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum context)
{
    PageParamDescriptor* page = genericTrackerDescribeInContextBegin(desc, context);
    
    
    // description common to all trackers
    genericTrackerDescribePointParameters(desc, page);
    // center
    {
        OFX::Double2DParamDescriptor* param = desc.defineDouble2DParam(kParamTrackingCenterPoint);
        param->setLabel(kParamTrackingCenterPointLabel);
        param->setHint(kParamTrackingCenterPointHint);
        param->setInstanceSpecific(true);
        param->setDoubleType(eDoubleTypeXYAbsolute);
        param->setDefaultCoordinateSystem(eCoordinatesNormalised);
        param->setDefault(0.5, 0.5);
        param->setRange(-DBL_MAX, -DBL_MAX, DBL_MAX, DBL_MAX); // Resolve requires range and display range or values are clamped to (-1,1)
        param->setDisplayRange(-10000, -10000, 10000, 10000); // Resolve requires display range or values are clamped to (-1,1)
        param->setIncrement(1.);
        param->setEvaluateOnChange(false); // The tracker is identity always
#     ifdef kOfxParamPropPluginMayWrite // removed from OFX 1.4
        param->getPropertySet().propSetInt(kOfxParamPropPluginMayWrite, 1, false);
#     endif
        if (page) {
            page->addChild(*param);
        }
    }
    
    // offset
    {
        OFX::Double2DParamDescriptor* param = desc.defineDouble2DParam(kParamTrackingOffset);
        param->setLabel(kParamTrackingOffsetLabel);
        param->setHint(kParamTrackingOffsetHint);
        param->setInstanceSpecific(true);
        param->setDoubleType(eDoubleTypeXYAbsolute);
        param->setDefaultCoordinateSystem(eCoordinatesCanonical);
        param->setDefault(0, 0);
        param->setRange(-DBL_MAX, -DBL_MAX, DBL_MAX, DBL_MAX); // Resolve requires range and display range or values are clamped to (-1,1)
        param->setDisplayRange(-10000, -10000, 10000, 10000); // Resolve requires display range or values are clamped to (-1,1)
        param->setIncrement(1.);
        param->setEvaluateOnChange(false); // The tracker is identity always
        if (page) {
            page->addChild(*param);
        }
    }
    
    // innerBtmLeft
    {
        OFX::Double2DParamDescriptor* param = desc.defineDouble2DParam(kParamTrackingPatternBoxBtmLeft);
        param->setLabel(kParamTrackingPatternBoxBtmLeftLabel);
        param->setHint(kParamTrackingPatternBoxBtmLeftHint);
        param->setDoubleType(eDoubleTypeXY);
        param->setDefaultCoordinateSystem(eCoordinatesCanonical);
        param->setDefault(-15,-15);
        param->setRange(-DBL_MAX, -DBL_MAX, DBL_MAX, DBL_MAX); // Resolve requires range and display range or values are clamped to (-1,1)
        param->setDisplayRange(-50., -50., 50., 50.);
        param->setIncrement(1.);
        //param->setIsSecret(true);
        param->setEvaluateOnChange(false); // The tracker is identity always
#     ifdef kOfxParamPropPluginMayWrite // removed from OFX 1.4
        param->getPropertySet().propSetInt(kOfxParamPropPluginMayWrite, 1, false);
#     endif
        if (page) {
            page->addChild(*param);
        }
    }
    
    // innerTopRight
    {
        OFX::Double2DParamDescriptor* param = desc.defineDouble2DParam(kParamTrackingPatternBoxTopRight);
        param->setLabel(kParamTrackingPatternBoxTopRightLabel);
        param->setHint(kParamTrackingPatternBoxTopRightHint);
        param->setDoubleType(eDoubleTypeXY);
        param->setDefaultCoordinateSystem(eCoordinatesCanonical);
        param->setDefault(15, 15);
        param->setRange(-DBL_MAX, -DBL_MAX, DBL_MAX, DBL_MAX); // Resolve requires range and display range or values are clamped to (-1,1)
        param->setDisplayRange(-50., -50., 50., 50.);
        param->setIncrement(1.);
        //innerTopRight->setIsSecret(true);
        param->setEvaluateOnChange(false); // The tracker is identity always
#     ifdef kOfxParamPropPluginMayWrite // removed from OFX 1.4
        param->getPropertySet().propSetInt(kOfxParamPropPluginMayWrite, 1, false);
#     endif
        if (page) {
            page->addChild(*param);
        }
    }
    
    // outerBtmLeft
    {
        OFX::Double2DParamDescriptor* param = desc.defineDouble2DParam(kParamTrackingSearchBoxBtmLeft);
        param->setLabel(kParamTrackingSearchBoxBtmLeftLabel);
        param->setHint(kParamTrackingSearchBoxBtmLeftHint);
        param->setDoubleType(eDoubleTypeXY);
        param->setDefaultCoordinateSystem(eCoordinatesCanonical);
        param->setDefault(-25,-25);
        param->setRange(-DBL_MAX, -DBL_MAX, DBL_MAX, DBL_MAX); // Resolve requires range and display range or values are clamped to (-1,1)
        param->setDisplayRange(-100., -100., 100., 100.);
        param->setIncrement(1.);
        //param->setIsSecret(true);
        param->setEvaluateOnChange(false); // The tracker is identity always
#     ifdef kOfxParamPropPluginMayWrite // removed from OFX 1.4
        param->getPropertySet().propSetInt(kOfxParamPropPluginMayWrite, 1, false);
#     endif
        if (page) {
            page->addChild(*param);
        }
    }
    
    // outerTopRight
    {
        OFX::Double2DParamDescriptor* param = desc.defineDouble2DParam(kParamTrackingSearchBoxTopRight);
        param->setLabel(kParamTrackingSearchBoxTopRightLabel);
        param->setHint(kParamTrackingSearchBoxBtmLeftHint);
        param->setDoubleType(eDoubleTypeXY);
        param->setDefaultCoordinateSystem(eCoordinatesCanonical);
        param->setDefault(25, 25);
        param->setRange(-DBL_MAX, -DBL_MAX, DBL_MAX, DBL_MAX); // Resolve requires range and display range or values are clamped to (-1,1)
        param->setDisplayRange(-100., -100., 100., 100.);
        param->setIncrement(1.);
        //param->setIsSecret(true);
        param->setEvaluateOnChange(false); // The tracker is identity always
#     ifdef kOfxParamPropPluginMayWrite // removed from OFX 1.4
        param->getPropertySet().propSetInt(kOfxParamPropPluginMayWrite, 1, false);
#     endif
        if (page) {
            page->addChild(*param);
        }
    }



    // this tracker can be masked
    if (context == eContextGeneral || (context == eContextPaint) || context == eContextTracker) {
        ClipDescriptor *maskClip = (context == eContextPaint) ? desc.defineClip("Brush") : desc.defineClip("Mask");
        maskClip->addSupportedComponent(ePixelComponentAlpha);
        maskClip->setTemporalClipAccess(false);
        if (context == eContextGeneral  || context == eContextTracker) {
            maskClip->setOptional(true);
        }
        maskClip->setSupportsTiles(kSupportsTiles);
        maskClip->setIsMask(true);
    }

    // score
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamScore);
        param->setLabel(kParamScoreLabel);
        param->setHint(kParamScoreHint);
        assert(param->getNOptions() == eTrackerSSD);
        param->appendOption(kParamScoreOptionSSD, kParamScoreOptionSSDHint);
        assert(param->getNOptions() == eTrackerSAD);
        param->appendOption(kParamScoreOptionSAD, kParamScoreOptionSADHint);
        assert(param->getNOptions() == eTrackerNCC);
        param->appendOption(kParamScoreOptionNCC, kParamScoreOptionNCCHint);
        assert(param->getNOptions() == eTrackerZNCC);
        param->appendOption(kParamScoreOptionZNCC, kParamScoreOptionZNCCHint);
        param->setDefault((int)eTrackerSAD);
        if (page) {
            page->addChild(*param);
        }
    }
}


OFX::ImageEffect* TrackerPMPluginFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum /*context*/)
{
    return new TrackerPMPlugin(handle);
}


static TrackerPMPluginFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
mRegisterPluginFactoryInstance(p)

OFXS_NAMESPACE_ANONYMOUS_EXIT
