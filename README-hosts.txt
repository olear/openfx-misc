Host-specific OpenFX bugs and caveats:

* DaVinci Resolve Lite

OFX API version 1.3
hostName=DaVinciResolveLite
hostLabel=DaVinci Resolve Lite
hostVersion=12.2.0 (12.2)
hostIsBackground=0
supportsOverlays=1
supportsMultiResolution=0
supportsTiles=0
temporalClipAccess=1
supportedComponents=OfxImageComponentRGBA,OfxImageComponentAlpha
supportedContexts=OfxImageEffectContextFilter,OfxImageEffectContextGeneral,OfxImageEffectContextTransition,OfxImageEffectContextGenerator
supportedPixelDepths=OfxBitDepthFloat,OfxBitDepthShort,OfxBitDepthByte
supportsMultipleClipDepths=0
supportsMultipleClipPARs=0
supportsSetableFrameRate=0
supportsSetableFielding=0
supportsStringAnimation=0
supportsCustomInteract=0
supportsChoiceAnimation=0
supportsBooleanAnimation=0
supportsCustomAnimation=0
supportsParametricAnimation=0
canTransform=0
maxParameters=-1
pageRowCount=0
pageColumnCount=0
isNatron=0
supportsDynamicChoices=0
supportsCascadingChoices=0
supportsChannelSelector=0
suites=OfxImageEffectSuite,OfxPropertySuite,OfxParameterSuite,OfxMemorySuite,OfxMessageSuite,OfxMessageSuiteV2,OfxProgressSuite,OfxTimeLineSuite

- version 11 of Resove Lite (from Mac App Store) does not support symbolic links in /Library/OFX/Plugins
- in Generators, even if the source clip is defined, it can not be fetched by the plug-in
- all defined clips will appear connected but give black and transparent (NULL) images. This is a problem for Mask clips, so a "Mask" boolean param must be added 
- kOfxImagePropField property is always kOfxImageFieldNone on OFX images
- OfxParameterSuiteV1::paramCopy doesn nothing, keys and values have to be copied explicitely (see CornerPin)
- The range AND display range has to be defined for all Double params (kOfxParamTypeDouble, kOfxParamTypeDouble2D, kOfxParamTypeDouble3D), or a default range of (-1,1) is used, and values cannot lie outsideof this range !
- The range AND display range has to be defined for Int params (kOfxParamTypeInteger), or a default range of (0,0) is used, and values cannot lie outsideof this range !
- kOfxParamPropDefaultCoordinateSystem=kOfxParamCoordinatesNormalised isn't supported (although API version 1.3 is claimed)
- kOfxParamTypeInteger2D kOfxParamTypeInteger3D are not supported (crash when opening the parameters page), at least in Generators

* Nuke

OFX API version 1.2
hostName=uk.co.thefoundry.nuke
hostLabel=nuke
hostVersion=9.0.6 (9.0v6)
hostIsBackground=0
supportsOverlays=1
supportsMultiResolution=1
supportsTiles=1
temporalClipAccess=1
supportedComponents=OfxImageComponentRGBA,OfxImageComponentAlpha,uk.co.thefoundry.OfxImageComponentMotionVectors,uk.co.thefoundry.OfxImageComponentStereoDisparity
supportedContexts=OfxImageEffectContextFilter,OfxImageEffectContextGeneral
supportedPixelDepths=OfxBitDepthFloat
supportsMultipleClipDepths=0
supportsMultipleClipPARs=0
supportsSetableFrameRate=0
supportsSetableFielding=0
supportsStringAnimation=0
supportsCustomInteract=1
supportsChoiceAnimation=1
supportsBooleanAnimation=1
supportsCustomAnimation=0
supportsParametricAnimation=0
canTransform=1
maxParameters=-1
pageRowCount=0
pageColumnCount=0
isNatron=0
supportsDynamicChoices=0
supportsCascadingChoices=0
supportsChannelSelector=0
suites=OfxImageEffectSuite,OfxPropertySuite,OfxParameterSuite,OfxMemorySuite,OfxMessageSuite,OfxMessageSuiteV2,OfxProgressSuite,OfxTimeLineSuite,OfxParametricParameterSuite,NukeOfxCameraSuite,uk.co.thefoundry.FnOfxImageEffectPlaneSuiteV1,uk.co.thefoundry.FnOfxImageEffectPlaneSuiteV2

- ChoiceParam items can only be set during description and cannot be changed afterwards
- Params that are described as secret can never be "revealed", they are doomed to remain secret (fix: set them as secret at the end of effect instance creation)

* Natron

OFX API version 1.3
hostName=fr.inria.Natron
hostLabel=Natron
hostVersion=2.0.0 (2.0.0)
hostIsBackground=0
supportsOverlays=1
supportsMultiResolution=1
supportsTiles=1
temporalClipAccess=1
supportedComponents=OfxImageComponentRGBA,OfxImageComponentAlpha,OfxImageComponentRGB,uk.co.thefoundry.OfxImageComponentMotionVectors,uk.co.thefoundry.OfxImageComponentStereoDisparity
supportedContexts=OfxImageEffectContextGenerator,OfxImageEffectContextFilter,OfxImageEffectContextGeneral,OfxImageEffectContextTransition
supportedPixelDepths=OfxBitDepthFloat,OfxBitDepthShort,OfxBitDepthByte
supportsMultipleClipDepths=1
supportsMultipleClipPARs=0
supportsSetableFrameRate=0
supportsSetableFielding=0
supportsStringAnimation=1
supportsCustomInteract=1
supportsChoiceAnimation=1
supportsBooleanAnimation=1
supportsCustomAnimation=1
supportsParametricAnimation=0
canTransform=1
maxParameters=-1
pageRowCount=0
pageColumnCount=0
isNatron=1
supportsDynamicChoices=1
supportsCascadingChoices=1
supportsChannelSelector=1
suites=OfxImageEffectSuite,OfxPropertySuite,OfxParameterSuite,OfxMemorySuite,OfxMessageSuite,OfxMessageSuiteV2,OfxProgressSuite,OfxTimeLineSuite,OfxParametricParameterSuite,uk.co.thefoundry.FnOfxImageEffectPlaneSuiteV1,uk.co.thefoundry.FnOfxImageEffectPlaneSuiteV2,OfxVegasStereoscopicImageEffectSuite

- may give a fake hostName for plugins that don't officially support Natron, but sets an extra host property kNatronOfxHostIsNatron
- the isidentity action may point to a frame on the output clip, which is useful for generators and readers
