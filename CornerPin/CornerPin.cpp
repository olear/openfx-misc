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
 * OFX CornerPin plugin.
 */

/*
   Although the indications from nuke/fnOfxExtensions.h were followed, and the
   kFnOfxImageEffectActionGetTransform action was implemented in the Support
   library, that action is never called by the Nuke host, so it cannot be tested.
   The code is left here for reference or for further extension.

   There is also an open question about how the last plugin in a transform chain
   may get the concatenated transform from upstream, the untransformed source image,
   concatenate its own transform and apply the resulting transform in its render
   action. Should the host be doing this instead?
 */

#include <cmath>
#include <cfloat>
#include <vector>
#ifdef DEBUG
#include <iostream>
#endif

#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include "ofxsOGLTextRenderer.h"
#include "ofxsTransform3x3.h"

using namespace OFX;

OFXS_NAMESPACE_ANONYMOUS_ENTER

#define kPluginName "CornerPinOFX"
#define kPluginMaskedName "CornerPinMaskedOFX"
#define kPluginGrouping "Transform"
#define kPluginDescription \
    "Allows an image to fit another in translation, rotation and scale.\n" \
    "The resulting transform is a translation if 1 point is enabled, a " \
    "similarity if 2 are enabled, an affine transform if 3 are enabled, " \
    "and a homography if they are all enabled.\n" \
    "This plugin concatenates transforms."
#define kPluginIdentifier "net.sf.openfx.CornerPinPlugin"
#define kPluginMaskedIdentifier "net.sf.openfx.CornerPinMaskedPlugin"
#define kPluginVersionMajor 1 // Incrementing this number means that you have broken backwards compatibility of the plug-in.
#define kPluginVersionMinor 0 // Increment this when you have fixed a bug or made it faster.

#define POINT_SIZE 5
#define POINT_TOLERANCE 6

#define kGroupTo "to"
#define kGroupToLabel "To"
static const char* const kParamTo[4] = {
    "to1",
    "to2",
    "to3",
    "to4"
};
static const char* const kParamEnable[4] = {
    "enable1",
    "enable2",
    "enable3",
    "enable4"
};
#define kParamEnableHint "Enables the point on the left."

#define kGroupFrom "from"
#define kGroupFromLabel "From"
static const char* const kParamFrom[4] = {
    "from1",
    "from2",
    "from3",
    "from4"
};


#define kParamCopyFrom "copyFrom"
#define kParamCopyFromLabel "Copy \"From\" points"
#define kParamCopyFromHint "Copy the content from the \"to\" points to the \"from\" points."

#define kParamCopyTo "copyTo"
#define kParamCopyToLabel "Copy \"To\" points"
#define kParamCopyToHint "Copy the content from the \"from\" points to the \"to\" points."

#define kParamCopyInputRoD "setToInputRod"
#define kParamCopyInputRoDLabel "Set to input rod"
#define kParamCopyInputRoDHint "Copy the values from the source region of definition into the \"to\" points."

#define kParamOverlayPoints "overlayPoints"
#define kParamOverlayPointsLabel "Overlay Points"
#define kParamOverlayPointsHint "Whether to display the \"from\" or the \"to\" points in the overlay"
#define kParamOverlayPointsOptionTo "To"
#define kParamOverlayPointsOptionFrom "From"

#define kGroupExtraMatrix "transformMatrix"
#define kGroupExtraMatrixLabel "Extra Matrix"
#define kGroupExtraMatrixHint "This matrix gets concatenated to the transform defined by the other parameters."
#define kParamExtraMatrixRow1 "row1"
#define kParamExtraMatrixRow2 "row2"
#define kParamExtraMatrixRow3 "row3"

#define kParamTransformInteractive "interactive"
#define kParamTransformInteractiveLabel "Interactive Update"
#define kParamTransformInteractiveHint "If checked, update the parameter values during interaction with the image viewer, else update the values when pen is released."

#define kParamSrcClipChanged "srcClipChanged"

#define POINT_INTERACT_LINE_SIZE_PIXELS 20


////////////////////////////////////////////////////////////////////////////////
/** @brief The plugin that does our work */
class CornerPinPlugin
    : public Transform3x3Plugin
{
public:
    /** @brief ctor */
    CornerPinPlugin(OfxImageEffectHandle handle,
                    bool masked)
        : Transform3x3Plugin(handle, masked, OFX::Transform3x3Plugin::eTransform3x3ParamsTypeMotionBlur)
        , _extraMatrixRow1(0)
        , _extraMatrixRow2(0)
        , _extraMatrixRow3(0)
        , _copyFromButton(0)
        , _copyToButton(0)
        , _copyInputButton(0)
        , _srcClipChanged(0)
    {
        // NON-GENERIC
        for (int i = 0; i < 4; ++i) {
            _to[i] = fetchDouble2DParam(kParamTo[i]);
            _enable[i] = fetchBooleanParam(kParamEnable[i]);
            _from[i] = fetchDouble2DParam(kParamFrom[i]);
            assert(_to[i] && _enable[i] && _from[i]);
        }

        _extraMatrixRow1 = fetchDouble3DParam(kParamExtraMatrixRow1);
        _extraMatrixRow2 = fetchDouble3DParam(kParamExtraMatrixRow2);
        _extraMatrixRow3 = fetchDouble3DParam(kParamExtraMatrixRow3);
        assert(_extraMatrixRow1 && _extraMatrixRow2 && _extraMatrixRow3);

        _copyFromButton = fetchPushButtonParam(kParamCopyFrom);
        _copyToButton = fetchPushButtonParam(kParamCopyTo);
        _copyInputButton = fetchPushButtonParam(kParamCopyInputRoD);
        assert(_copyInputButton && _copyToButton && _copyFromButton);
        _srcClipChanged = fetchBooleanParam(kParamSrcClipChanged);
        assert(_srcClipChanged);
    }

private:

    OFX::Matrix3x3 getExtraMatrix(OfxTime time) const
    {
        OFX::Matrix3x3 ret;

        _extraMatrixRow1->getValueAtTime(time, ret.a, ret.b, ret.c);
        _extraMatrixRow2->getValueAtTime(time, ret.d, ret.e, ret.f);
        _extraMatrixRow3->getValueAtTime(time, ret.g, ret.h, ret.i);

        return ret;
    }

    bool getHomography(OfxTime time, const OfxPointD & scale,
                       bool inverseTransform,
                       const OFX::Point3D & p1,
                       const OFX::Point3D & p2,
                       const OFX::Point3D & p3,
                       const OFX::Point3D & p4,
                       OFX::Matrix3x3 & m);
    virtual bool isIdentity(double time) OVERRIDE FINAL;
    virtual bool getInverseTransformCanonical(double time, int view, double amount, bool invert, OFX::Matrix3x3* invtransform) const OVERRIDE FINAL;
    virtual void changedParam(const OFX::InstanceChangedArgs &args, const std::string &paramName) OVERRIDE FINAL;

    /** @brief called when a clip has just been changed in some way (a rewire maybe) */
    virtual void changedClip(const InstanceChangedArgs &args, const std::string &clipName) OVERRIDE FINAL;

private:
    // NON-GENERIC
    OFX::Double2DParam* _to[4];
    OFX::BooleanParam* _enable[4];
    OFX::Double3DParam* _extraMatrixRow1;
    OFX::Double3DParam* _extraMatrixRow2;
    OFX::Double3DParam* _extraMatrixRow3;
    OFX::Double2DParam* _from[4];
    OFX::PushButtonParam* _copyFromButton;
    OFX::PushButtonParam* _copyToButton;
    OFX::PushButtonParam* _copyInputButton;
    OFX::BooleanParam* _srcClipChanged; // set to true the first time the user connects src
};


bool
CornerPinPlugin::getInverseTransformCanonical(OfxTime time,
                                              int /*view*/,
                                              double amount,
                                              bool invert,
                                              OFX::Matrix3x3* invtransform) const
{
    // in this new version, both from and to are enableds/disabled at the same time
    bool enable[4];
    OFX::Point3D p[2][4];
    int f = invert ? 0 : 1;
    int t = invert ? 1 : 0;
    int k = 0;

    for (int i = 0; i < 4; ++i) {
        _enable[i]->getValueAtTime(time, enable[i]);
        if (enable[i]) {
            _from[i]->getValueAtTime(time, p[f][k].x, p[f][k].y);
            _to[i]->getValueAtTime(time, p[t][k].x, p[t][k].y);
            ++k;
        }
        p[0][i].z = p[1][i].z = 1.;
    }

    if (amount != 1.) {
        int k = 0;
        for (int i = 0; i < 4; ++i) {
            if (enable[i]) {
                p[t][k].x = p[f][k].x + amount * (p[t][k].x - p[f][k].x);
                p[t][k].y = p[f][k].y + amount * (p[t][k].y - p[f][k].y);
                ++k;
            }
        }
    }

    // k contains the number of valid points
    OFX::Matrix3x3 homo3x3;
    bool success = false;

    assert(0 <= k && k <= 4);
    if (k == 0) {
        // no points, only apply extraMat;
        *invtransform = getExtraMatrix(time);

        return true;
    }

    switch (k) {
    case 4:
        success = homo3x3.setHomographyFromFourPoints(p[0][0], p[0][1], p[0][2], p[0][3], p[1][0], p[1][1], p[1][2], p[1][3]);
        break;
    case 3:
        success = homo3x3.setAffineFromThreePoints(p[0][0], p[0][1], p[0][2], p[1][0], p[1][1], p[1][2]);
        break;
    case 2:
        success = homo3x3.setSimilarityFromTwoPoints(p[0][0], p[0][1], p[1][0], p[1][1]);
        break;
    case 1:
        success = homo3x3.setTranslationFromOnePoint(p[0][0], p[1][0]);
        break;
    }
    if (!success) {
        ///cannot compute the homography when 3 points are aligned
        return false;
    }

    OFX::Matrix3x3 extraMat = getExtraMatrix(time);
    *invtransform = homo3x3 * extraMat;

    return true;
} // CornerPinPlugin::getInverseTransformCanonical

// overridden is identity
bool
CornerPinPlugin::isIdentity(double time)
{
    OFX::Matrix3x3 extraMat = getExtraMatrix(time);

    if ( !extraMat.isIdentity() ) {
        return false;
    }

    // extraMat is identity.

    // The transform is identity either if no point is enabled, or if
    // all enabled from's are equal to their counterpart to
    for (int i = 0; i < 4; ++i) {
        bool enable;
        _enable[i]->getValueAtTime(time, enable);
        if (enable) {
            OfxPointD p, q;
            _from[i]->getValueAtTime(time, p.x, p.y);
            _to[i]->getValueAtTime(time, q.x, q.y);
            if ( (p.x != q.x) || (p.y != q.y) ) {
                return false;
            }
        }
    }

    return true;
}

static void
copyPoint(OFX::Double2DParam* from,
          OFX::Double2DParam* to)
{
    // because some hosts (e.g. Resolve) have a faulty paramCopy, we first copy
    // all keys and values
    OfxPointD p;
    unsigned int keyCount = from->getNumKeys();

    to->deleteAllKeys();
    for (unsigned int i = 0; i < keyCount; ++i) {
        OfxTime time = from->getKeyTime(i);
        from->getValueAtTime(time, p.x, p.y);
        to->setValueAtTime(time, p.x, p.y);
    }
    if (keyCount == 0) {
        from->getValue(p.x, p.y);
        to->setValue(p.x, p.y);
    }
    // OfxParameterSuiteV1::paramCopy (does not work under Resolve)
    to->copyFrom(*from, 0, NULL);
}

void
CornerPinPlugin::changedParam(const OFX::InstanceChangedArgs &args,
                              const std::string &paramName)
{
    // If any parameter is set by the user, set srcClipChanged to true so that from/to is not reset when
    // connecting the input.
    //printf("srcClipChanged=%s\n", _srcClipChanged->getValue() ? "true" : "false");
    if (paramName == kParamCopyInputRoD) {
        if ( _srcClip && _srcClip->isConnected() ) {
            beginEditBlock(paramName);
            const OfxRectD & srcRoD = _srcClip->getRegionOfDefinition(args.time);
            _from[0]->setValue(srcRoD.x1, srcRoD.y1);
            _from[1]->setValue(srcRoD.x2, srcRoD.y1);
            _from[2]->setValue(srcRoD.x2, srcRoD.y2);
            _from[3]->setValue(srcRoD.x1, srcRoD.y2);
            changedTransform(args);
            if ( (args.reason == OFX::eChangeUserEdit) && !_srcClipChanged->getValue() ) {
                _srcClipChanged->setValue(true);
            }
            endEditBlock();
        }
    } else if (paramName == kParamCopyFrom) {
        beginEditBlock(paramName);
        for (int i = 0; i < 4; ++i) {
            copyPoint(_from[i], _to[i]);
        }
        changedTransform(args);
        if ( (args.reason == OFX::eChangeUserEdit) && !_srcClipChanged->getValue() ) {
            _srcClipChanged->setValue(true);
        }
        endEditBlock();
    } else if (paramName == kParamCopyTo) {
        beginEditBlock(paramName);
        for (int i = 0; i < 4; ++i) {
            copyPoint(_to[i], _from[i]);
        }
        changedTransform(args);
        if ( (args.reason == OFX::eChangeUserEdit) && !_srcClipChanged->getValue() ) {
            _srcClipChanged->setValue(true);
        }
        endEditBlock();
    } else if ( (paramName == kParamTo[0]) ||
                ( paramName == kParamTo[1]) ||
                ( paramName == kParamTo[2]) ||
                ( paramName == kParamTo[3]) ||
                ( paramName == kParamEnable[0]) ||
                ( paramName == kParamEnable[1]) ||
                ( paramName == kParamEnable[2]) ||
                ( paramName == kParamEnable[3]) ||
                ( paramName == kParamFrom[0]) ||
                ( paramName == kParamFrom[1]) ||
                ( paramName == kParamFrom[2]) ||
                ( paramName == kParamFrom[3]) ||
                ( paramName == kParamExtraMatrixRow1) ||
                ( paramName == kParamExtraMatrixRow2) ||
                ( paramName == kParamExtraMatrixRow3) ) {
        beginEditBlock(paramName);
        changedTransform(args);
        if ( (args.reason == OFX::eChangeUserEdit) && !_srcClipChanged->getValue() ) {
            _srcClipChanged->setValue(true);
        }
        endEditBlock();
    } else {
        Transform3x3Plugin::changedParam(args, paramName);
    }
} // CornerPinPlugin::changedParam

void
CornerPinPlugin::changedClip(const InstanceChangedArgs &args,
                             const std::string &clipName)
{
    if ( (clipName == kOfxImageEffectSimpleSourceClipName) &&
         _srcClip && _srcClip->isConnected() &&
         !_srcClipChanged->getValue() &&
         ( args.reason == OFX::eChangeUserEdit) ) {
        const OfxRectD & srcRoD = _srcClip->getRegionOfDefinition(args.time);
        beginEditBlock(clipName);
        _from[0]->setValue(srcRoD.x1, srcRoD.y1);
        _from[1]->setValue(srcRoD.x2, srcRoD.y1);
        _from[2]->setValue(srcRoD.x2, srcRoD.y2);
        _from[3]->setValue(srcRoD.x1, srcRoD.y2);
        _to[0]->setValue(srcRoD.x1, srcRoD.y1);
        _to[1]->setValue(srcRoD.x2, srcRoD.y1);
        _to[2]->setValue(srcRoD.x2, srcRoD.y2);
        _to[3]->setValue(srcRoD.x1, srcRoD.y2);
        changedTransform(args);
        if ( (args.reason == OFX::eChangeUserEdit) && !_srcClipChanged->getValue() ) {
            _srcClipChanged->setValue(true);
        }
        endEditBlock();
    }
}

class CornerPinTransformInteract
    : public OFX::OverlayInteract
{
public:

    CornerPinTransformInteract(OfxInteractHandle handle,
                               OFX::ImageEffect* effect)
        : OFX::OverlayInteract(handle)
        , _plugin( dynamic_cast<CornerPinPlugin*>(effect) )
        , _invert(0)
        , _overlayPoints(0)
        , _interactive(0)
        , _dragging(-1)
        , _hovering(-1)
        , _lastMousePos()
    {
        assert(_plugin);
        for (int i = 0; i < 4; ++i) {
            _to[i] = _plugin->fetchDouble2DParam(kParamTo[i]);
            _from[i] = _plugin->fetchDouble2DParam(kParamFrom[i]);
            _enable[i] = _plugin->fetchBooleanParam(kParamEnable[i]);
            assert(_to[i] && _from[i] && _enable[i]);
            addParamToSlaveTo(_to[i]);
            addParamToSlaveTo(_from[i]);
            addParamToSlaveTo(_enable[i]);
        }
        _invert = _plugin->fetchBooleanParam(kParamTransform3x3Invert);
        addParamToSlaveTo(_invert);
        _overlayPoints = _plugin->fetchChoiceParam(kParamOverlayPoints);
        addParamToSlaveTo(_overlayPoints);
        _interactive = _plugin->fetchBooleanParam(kParamTransformInteractive);
        assert(_invert && _overlayPoints && _interactive);

        for (int i = 0; i < 4; ++i) {
            _toDrag[i].x = _toDrag[i].y = 0;
            _fromDrag[i].x = _fromDrag[i].y = 0;
            _enableDrag[i] = false;
        }
        _useFromDrag = false;
        _interactiveDrag = false;
    }

    // overridden functions from OFX::Interact to do things
    virtual bool draw(const OFX::DrawArgs &args) OVERRIDE FINAL;
    virtual bool penMotion(const OFX::PenArgs &args) OVERRIDE FINAL;
    virtual bool penDown(const OFX::PenArgs &args) OVERRIDE FINAL;
    virtual bool penUp(const OFX::PenArgs &args) OVERRIDE FINAL;
    //virtual bool keyDown(const OFX::KeyArgs &args) OVERRIDE FINAL;
    //virtual bool keyUp(const OFX::KeyArgs &args) OVERRIDE FINAL;
    virtual void loseFocus(const FocusArgs &args) OVERRIDE FINAL;

private:

    /**
     * @brief Returns true if the points that should be used by the overlay are
     * the "from" points, otherwise the overlay is assumed to use the "to" points.
     **/
    /*
       bool isFromPoints(double time) const
       {
        int v;
        _overlayPoints->getValueAtTime(time, v);
        return v == 1;
       }
     */
    CornerPinPlugin* _plugin;
    OFX::Double2DParam* _to[4];
    OFX::Double2DParam* _from[4];
    OFX::BooleanParam* _enable[4];
    OFX::BooleanParam* _invert;
    OFX::ChoiceParam* _overlayPoints;
    OFX::BooleanParam* _interactive;
    int _dragging; // -1: idle, else dragging point number
    int _hovering; // -1: idle, else hovering point number
    OfxPointD _lastMousePos;
    OfxPointD _toDrag[4];
    OfxPointD _fromDrag[4];
    bool _enableDrag[4];
    bool _useFromDrag;
    bool _interactiveDrag;
};

static bool
isNearby(const OfxPointD & p,
         double x,
         double y,
         double tolerance,
         const OfxPointD & pscale)
{
    return std::fabs(p.x - x) <= tolerance * pscale.x &&  std::fabs(p.y - y) <= tolerance * pscale.y;
}

bool
CornerPinTransformInteract::draw(const OFX::DrawArgs &args)
{
#if 0 //def DEBUG
    const OfxPointD &pscale = args.pixelScale;
    // kOfxInteractPropPixelScale gives the size of a screen pixel in canonical coordinates.
    // - it is correct under Nuke and Natron
    // - it is always (1,1) under Resolve
    std::cout << "pixelScale: " << pscale.x << ',' << pscale.y << std::endl;
#endif
    const double time = args.time;
    OfxRGBColourD color = {
        0.8, 0.8, 0.8
    };
    getSuggestedColour(color);
    GLdouble projection[16];
    glGetDoublev( GL_PROJECTION_MATRIX, projection);
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    OfxPointD shadow; // how much to translate GL_PROJECTION to get exactly one pixel on screen
    shadow.x = 2. / (projection[0] * viewport[2]);
    shadow.y = 2. / (projection[5] * viewport[3]);

    OfxPointD to[4];
    OfxPointD from[4];
    bool enable[4];
    bool useFrom;

    if (_dragging == -1) {
        for (int i = 0; i < 4; ++i) {
            _to[i]->getValueAtTime(time, to[i].x, to[i].y);
            _from[i]->getValueAtTime(time, from[i].x, from[i].y);
            _enable[i]->getValueAtTime(time, enable[i]);
        }
        int v;
        _overlayPoints->getValueAtTime(time, v);
        useFrom = (v == 1);
    } else {
        for (int i = 0; i < 4; ++i) {
            to[i] = _toDrag[i];
            from[i] = _fromDrag[i];
            enable[i] = _enableDrag[i];
        }
        useFrom = _useFromDrag;
    }

    OfxPointD p[4];
    OfxPointD q[4];
    int enableBegin = 4;
    int enableEnd = 0;
    for (int i = 0; i < 4; ++i) {
        if (enable[i]) {
            if (useFrom) {
                p[i] = from[i];
                q[i] = to[i];
            } else {
                q[i] = from[i];
                p[i] = to[i];
            }
            if (i < enableBegin) {
                enableBegin = i;
            }
            if (i + 1 > enableEnd) {
                enableEnd = i + 1;
            }
        }
    }

    //glPushAttrib(GL_ALL_ATTRIB_BITS); // caller is responsible for protecting attribs

    //glDisable(GL_LINE_STIPPLE);
    glEnable(GL_LINE_SMOOTH);
    //glEnable(GL_POINT_SMOOTH);
    glEnable(GL_BLEND);
    glHint(GL_LINE_SMOOTH_HINT, GL_DONT_CARE);
    glLineWidth(1.5f);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glPointSize(POINT_SIZE);
    // Draw everything twice
    // l = 0: shadow
    // l = 1: drawing
    for (int l = 0; l < 2; ++l) {
        // shadow (uses GL_PROJECTION)
        glMatrixMode(GL_PROJECTION);
        int direction = (l == 0) ? 1 : -1;
        // translate (1,-1) pixels
        glTranslated(direction * shadow.x, -direction * shadow.y, 0);
        glMatrixMode(GL_MODELVIEW); // Modelview should be used on Nuke

        glColor3f( (float)(color.r / 2) * l, (float)(color.g / 2) * l, (float)(color.b / 2) * l );
        glBegin(GL_LINES);
        for (int i = enableBegin; i < enableEnd; ++i) {
            if (enable[i]) {
                glVertex2d(p[i].x, p[i].y);
                glVertex2d(q[i].x, q[i].y);
            }
        }
        glEnd();
        glColor3f( (float)color.r * l, (float)color.g * l, (float)color.b * l );
        glBegin(GL_LINE_LOOP);
        for (int i = enableBegin; i < enableEnd; ++i) {
            if (enable[i]) {
                glVertex2d(p[i].x, p[i].y);
            }
        }
        glEnd();
        glBegin(GL_POINTS);
        for (int i = enableBegin; i < enableEnd; ++i) {
            if (enable[i]) {
                if ( (_hovering == i) || (_dragging == i) ) {
                    glColor3f(0.f * l, 1.f * l, 0.f * l);
                } else {
                    glColor3f( (float)color.r * l, (float)color.g * l, (float)color.b * l );
                }
                glVertex2d(p[i].x, p[i].y);
            }
        }
        glEnd();
        glColor3f( (float)color.r * l, (float)color.g * l, (float)color.b * l );
        for (int i = enableBegin; i < enableEnd; ++i) {
            if (enable[i]) {
                TextRenderer::bitmapString(p[i].x, p[i].y, useFrom ? kParamFrom[i] : kParamTo[i]);
            }
        }
    }

    //glPopAttrib();

    return true;
} // CornerPinTransformInteract::draw

bool
CornerPinTransformInteract::penMotion(const OFX::PenArgs &args)
{
    const OfxPointD &pscale = args.pixelScale;
    const double time = args.time;
    OfxPointD to[4];
    OfxPointD from[4];
    bool enable[4];
    bool useFrom;

    if (_dragging == -1) { // mouse is released
        for (int i = 0; i < 4; ++i) {
            _to[i]->getValueAtTime(time, to[i].x, to[i].y);
            _from[i]->getValueAtTime(time, from[i].x, from[i].y);
            _enable[i]->getValueAtTime(time, enable[i]);
        }
        int v;
        _overlayPoints->getValueAtTime(time, v);
        useFrom = (v == 1);
    } else {
        for (int i = 0; i < 4; ++i) {
            to[i] = _toDrag[i];
            from[i] = _fromDrag[i];
            enable[i] = _enableDrag[i];
        }
        useFrom = _useFromDrag;
    }

    OfxPointD p[4];
    //OfxPointD q[4];
    int enableBegin = 4;
    int enableEnd = 0;
    for (int i = 0; i < 4; ++i) {
        if (enable[i]) {
            if (useFrom) {
                p[i] = from[i];
                //q[i] = to[i];
            } else {
                //q[i] = from[i];
                p[i] = to[i];
            }
            if (i < enableBegin) {
                enableBegin = i;
            }
            if (i + 1 > enableEnd) {
                enableEnd = i + 1;
            }
        }
    }

    bool didSomething = false;
    bool valuesChanged = false;
    OfxPointD delta;
    delta.x = args.penPosition.x - _lastMousePos.x;
    delta.y = args.penPosition.y - _lastMousePos.y;

    _hovering = -1;

    for (int i = enableBegin; i < enableEnd; ++i) {
        if (enable[i]) {
            if (_dragging == i) {
                if (useFrom) {
                    from[i].x += delta.x;
                    from[i].y += delta.y;
                    _fromDrag[i] = from[i];
                } else {
                    to[i].x += delta.x;
                    to[i].y += delta.y;
                    _toDrag[i] = to[i];
                }
                valuesChanged = true;
            } else if ( isNearby(args.penPosition, p[i].x, p[i].y, POINT_TOLERANCE, pscale) ) {
                _hovering = i;
                didSomething = true;
            }
        }
    }

    if ( (_dragging != -1) && _interactiveDrag && valuesChanged ) {
        // no need to redraw overlay since it is slave to the paramaters
        if (useFrom) {
            _from[_dragging]->setValue(from[_dragging].x, from[_dragging].y);
        } else {
            _to[_dragging]->setValue(to[_dragging].x, to[_dragging].y);
        }
    } else if (didSomething || valuesChanged) {
        _effect->redrawOverlays();
    }

    _lastMousePos = args.penPosition;

    return didSomething || valuesChanged;
} // CornerPinTransformInteract::penMotion

bool
CornerPinTransformInteract::penDown(const OFX::PenArgs &args)
{
    const OfxPointD &pscale = args.pixelScale;
    const double time = args.time;
    OfxPointD to[4];
    OfxPointD from[4];
    bool enable[4];
    bool useFrom;

    if (_dragging == -1) {
        for (int i = 0; i < 4; ++i) {
            _to[i]->getValueAtTime(time, to[i].x, to[i].y);
            _from[i]->getValueAtTime(time, from[i].x, from[i].y);
            _enable[i]->getValueAtTime(time, enable[i]);
        }
        int v;
        _overlayPoints->getValueAtTime(time, v);
        useFrom = (v == 1);
        if (_interactive) {
            _interactive->getValueAtTime(args.time, _interactiveDrag);
        }
    } else {
        for (int i = 0; i < 4; ++i) {
            to[i] = _toDrag[i];
            from[i] = _fromDrag[i];
            enable[i] = _enableDrag[i];
        }
        useFrom = _useFromDrag;
    }

    OfxPointD p[4];
    //OfxPointD q[4];
    int enableBegin = 4;
    int enableEnd = 0;
    for (int i = 0; i < 4; ++i) {
        if (enable[i]) {
            if (useFrom) {
                p[i] = from[i];
                //q[i] = to[i];
            } else {
                //q[i] = from[i];
                p[i] = to[i];
            }
            if (i < enableBegin) {
                enableBegin = i;
            }
            if (i + 1 > enableEnd) {
                enableEnd = i + 1;
            }
        }
    }

    bool didSomething = false;

    for (int i = enableBegin; i < enableEnd; ++i) {
        if (enable[i]) {
            if ( isNearby(args.penPosition, p[i].x, p[i].y, POINT_TOLERANCE, pscale) ) {
                _dragging = i;
                didSomething = true;
            }
            _toDrag[i] = to[i];
            _fromDrag[i] = from[i];
            _enableDrag[i] = enable[i];
        }
    }
    _useFromDrag = useFrom;

    if (didSomething) {
        _effect->redrawOverlays();
    }

    _lastMousePos = args.penPosition;

    return didSomething;
} // CornerPinTransformInteract::penDown

bool
CornerPinTransformInteract::penUp(const OFX::PenArgs & /*args*/)
{
    bool didSomething = _dragging != -1;

    if ( !_interactiveDrag && (_dragging != -1) ) {
        // no need to redraw overlay since it is slave to the paramaters
        if (_useFromDrag) {
            _from[_dragging]->setValue(_fromDrag[_dragging].x, _fromDrag[_dragging].y);
        } else {
            _to[_dragging]->setValue(_toDrag[_dragging].x, _toDrag[_dragging].y);
        }
    } else if (didSomething) {
        _effect->redrawOverlays();
    }

    _dragging = -1;

    return didSomething;
}

/** @brief Called when the interact is loses input focus */
void
CornerPinTransformInteract::loseFocus(const FocusArgs & /*args*/)
{
    _dragging = -1;
    _hovering = -1;
    _interactiveDrag = false;
}


class CornerPinOverlayDescriptor
    : public DefaultEffectOverlayDescriptor<CornerPinOverlayDescriptor, CornerPinTransformInteract>
{
};

static void
defineCornerPinToDouble2DParam(OFX::ImageEffectDescriptor &desc,
                               PageParamDescriptor *page,
                               GroupParamDescriptor* group,
                               int i,
                               double x,
                               double y)
{
    // size
    {
        Double2DParamDescriptor* param = desc.defineDouble2DParam(kParamTo[i]);
        param->setLabel(kParamTo[i]);
        param->setDoubleType(OFX::eDoubleTypeXYAbsolute);
        param->setAnimates(true);
        param->setIncrement(1.);
        param->setRange(-DBL_MAX, -DBL_MAX, DBL_MAX, DBL_MAX);
        param->setDisplayRange(-10000, -10000, 10000, 10000); // Resolve requires display range or values are clamped to (-1,1)
        param->setDefaultCoordinateSystem(OFX::eCoordinatesNormalised);
        param->setDefault(x, y);
        param->setDimensionLabels("x", "y");
        param->setLayoutHint(OFX::eLayoutHintNoNewLine, 1);
        if (group) {
            param->setParent(*group);
        }
        if (page) {
            page->addChild(*param);
        }
    }

    // enable
    {
        BooleanParamDescriptor* param = desc.defineBooleanParam(kParamEnable[i]);
        param->setLabel(kParamEnable[i]);
        param->setDefault(true);
        param->setAnimates(true);
        param->setHint(kParamEnableHint);
        param->setParent(*group);
        if (page) {
            page->addChild(*param);
        }
    }
}

static void
defineCornerPinFromsDouble2DParam(OFX::ImageEffectDescriptor &desc,
                                  PageParamDescriptor *page,
                                  GroupParamDescriptor* group,
                                  int i,
                                  double x,
                                  double y)
{
    Double2DParamDescriptor* param = desc.defineDouble2DParam(kParamFrom[i]);

    param->setLabel(kParamFrom[i]);
    param->setDoubleType(OFX::eDoubleTypeXYAbsolute);
    param->setAnimates(true);
    param->setIncrement(1.);
    param->setRange(-DBL_MAX, -DBL_MAX, DBL_MAX, DBL_MAX); // Resolve requires range and display range or values are clamped to (-1,1)
    param->setDisplayRange(-10000, -10000, 10000, 10000);
    param->setDefaultCoordinateSystem(OFX::eCoordinatesNormalised);
    param->setDefault(x, y);
    param->setDimensionLabels("x", "y");
    param->setParent(*group);
    if (page) {
        page->addChild(*param);
    }
}

static void
defineExtraMatrixRow(OFX::ImageEffectDescriptor &desc,
                     PageParamDescriptor *page,
                     GroupParamDescriptor* group,
                     const std::string & name,
                     double x,
                     double y,
                     double z)
{
    Double3DParamDescriptor* param = desc.defineDouble3DParam(name);

    param->setLabel("");
    param->setAnimates(true);
    param->setDefault(x, y, z);
    param->setIncrement(0.01);
    if (group) {
        param->setParent(*group);
    }
    if (page) {
        page->addChild(*param);
    }
}

static void
CornerPinPluginDescribeInContext(OFX::ImageEffectDescriptor &desc,
                                 OFX::ContextEnum /*context*/,
                                 PageParamDescriptor *page)
{
    // NON-GENERIC PARAMETERS
    //
    // toPoints
    {
        GroupParamDescriptor* group = desc.defineGroupParam(kGroupTo);
        if (group) {
            group->setLabel(kGroupTo);
            group->setAsTab();
        }

        defineCornerPinToDouble2DParam(desc, page, group, 0, 0, 0);
        defineCornerPinToDouble2DParam(desc, page, group, 1, 1, 0);
        defineCornerPinToDouble2DParam(desc, page, group, 2, 1, 1);
        defineCornerPinToDouble2DParam(desc, page, group, 3, 0, 1);

        // copyFrom
        {
            PushButtonParamDescriptor* param = desc.definePushButtonParam(kParamCopyFrom);
            param->setLabel(kParamCopyFromLabel);
            param->setHint(kParamCopyFromHint);
            param->setParent(*group);
            if (page) {
                page->addChild(*param);
            }
        }

        if (page) {
            page->addChild(*group);
        }
    }

    // fromPoints
    {
        GroupParamDescriptor* group = desc.defineGroupParam(kGroupFrom);
        if (group) {
            group->setLabel(kGroupFrom);
            group->setAsTab();
        }

        defineCornerPinFromsDouble2DParam(desc, page, group, 0, 0, 0);
        defineCornerPinFromsDouble2DParam(desc, page, group, 1, 1, 0);
        defineCornerPinFromsDouble2DParam(desc, page, group, 2, 1, 1);
        defineCornerPinFromsDouble2DParam(desc, page, group, 3, 0, 1);

        // setToInput
        {
            PushButtonParamDescriptor* param = desc.definePushButtonParam(kParamCopyInputRoD);
            param->setLabel(kParamCopyInputRoDLabel);
            param->setHint(kParamCopyInputRoDHint);
            param->setLayoutHint(OFX::eLayoutHintNoNewLine, 1);
            if (group) {
                param->setParent(*group);
            }
            if (page) {
                page->addChild(*param);
            }
        }

        // copyTo
        {
            PushButtonParamDescriptor* param = desc.definePushButtonParam(kParamCopyTo);
            param->setLabel(kParamCopyToLabel);
            param->setHint(kParamCopyToHint);
            if (group) {
                param->setParent(*group);
            }
            if (page) {
                page->addChild(*param);
            }
        }

        if (page && group) {
            page->addChild(*group);
        }
    }

    // extraMatrix
    {
        GroupParamDescriptor* group = desc.defineGroupParam(kGroupExtraMatrix);
        if (group) {
            group->setLabel(kGroupExtraMatrixLabel);
            group->setHint(kGroupExtraMatrixHint);
            group->setOpen(false);
        }

        defineExtraMatrixRow(desc, page, group, kParamExtraMatrixRow1, 1, 0, 0);
        defineExtraMatrixRow(desc, page, group, kParamExtraMatrixRow2, 0, 1, 0);
        defineExtraMatrixRow(desc, page, group, kParamExtraMatrixRow3, 0, 0, 1);

        if (page && group) {
            page->addChild(*group);
        }
    }

    // overlayPoints
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamOverlayPoints);
        param->setLabel(kParamOverlayPointsLabel);
        param->setHint(kParamOverlayPointsHint);
        param->appendOption(kParamOverlayPointsOptionTo);
        param->appendOption(kParamOverlayPointsOptionFrom);
        param->setDefault(0);
        param->setEvaluateOnChange(false);
        if (page) {
            page->addChild(*param);
        }
    }

    // interactive
    {
        BooleanParamDescriptor* param = desc.defineBooleanParam(kParamTransformInteractive);
        param->setLabel(kParamTransformInteractiveLabel);
        param->setHint(kParamTransformInteractiveHint);
        param->setEvaluateOnChange(false);
        if (page) {
            page->addChild(*param);
        }
    }
} // CornerPinPluginDescribeInContext

mDeclarePluginFactory(CornerPinPluginFactory, {}, {});
void
CornerPinPluginFactory::describe(OFX::ImageEffectDescriptor &desc)
{
    // basic labels
    desc.setLabel(kPluginName);
    desc.setPluginGrouping(kPluginGrouping);
    desc.setPluginDescription(kPluginDescription);

    Transform3x3Describe(desc, false);

    desc.setOverlayInteractDescriptor(new CornerPinOverlayDescriptor);
}

void
CornerPinPluginFactory::describeInContext(OFX::ImageEffectDescriptor &desc,
                                          OFX::ContextEnum context)
{
    // make some pages and to things in
    PageParamDescriptor *page = Transform3x3DescribeInContextBegin(desc, context, false);

    CornerPinPluginDescribeInContext(desc, context, page);

    Transform3x3DescribeInContextEnd(desc, context, page, false, OFX::Transform3x3Plugin::eTransform3x3ParamsTypeMotionBlur);

    // srcClipChanged
    {
        OFX::BooleanParamDescriptor* param = desc.defineBooleanParam(kParamSrcClipChanged);
        param->setDefault(false);
        param->setIsSecret(true);
        param->setAnimates(false);
        param->setEvaluateOnChange(false);
        page->addChild(*param);
    }
}

OFX::ImageEffect*
CornerPinPluginFactory::createInstance(OfxImageEffectHandle handle,
                                       OFX::ContextEnum /*context*/)
{
    return new CornerPinPlugin(handle, false);
}

mDeclarePluginFactory(CornerPinMaskedPluginFactory, {}, {});
void
CornerPinMaskedPluginFactory::describe(OFX::ImageEffectDescriptor &desc)
{
    // basic labels
    desc.setLabel(kPluginMaskedName);
    desc.setPluginGrouping(kPluginGrouping);
    desc.setPluginDescription(kPluginDescription);

    Transform3x3Describe(desc, true);

    desc.setOverlayInteractDescriptor(new CornerPinOverlayDescriptor);
}

void
CornerPinMaskedPluginFactory::describeInContext(OFX::ImageEffectDescriptor &desc,
                                                OFX::ContextEnum context)
{
    // make some pages and to things in
    PageParamDescriptor *page = Transform3x3DescribeInContextBegin(desc, context, true);

    CornerPinPluginDescribeInContext(desc, context, page);

    Transform3x3DescribeInContextEnd(desc, context, page, true, OFX::Transform3x3Plugin::eTransform3x3ParamsTypeMotionBlur);

    // srcClipChanged
    {
        OFX::BooleanParamDescriptor* param = desc.defineBooleanParam(kParamSrcClipChanged);
        param->setDefault(false);
        param->setIsSecret(true);
        param->setAnimates(false);
        param->setEvaluateOnChange(false);
        page->addChild(*param);
    }
}

OFX::ImageEffect*
CornerPinMaskedPluginFactory::createInstance(OfxImageEffectHandle handle,
                                             OFX::ContextEnum /*context*/)
{
    return new CornerPinPlugin(handle, true);
}

static CornerPinPluginFactory p1(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
static CornerPinMaskedPluginFactory p2(kPluginMaskedIdentifier, kPluginVersionMajor, kPluginVersionMinor);
mRegisterPluginFactoryInstance(p1)
mRegisterPluginFactoryInstance(p2)

OFXS_NAMESPACE_ANONYMOUS_EXIT
