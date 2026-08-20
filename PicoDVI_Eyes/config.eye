{
  // Single-eye PicoDVI build. Values not listed here keep the defaults
  // compiled into eye_config.h.
  //
  // Sizes are in SCREEN PIXELS and scale with displaySize. The original
  // 240px demon eye used eyeRadius 125 / irisRadius 110 / slitPupilRadius 100;
  // these are those values scaled by 176/240.

  "displaySize"     : 176,
  "eyeRadius"       : 93,
  "irisRadius"      : 80,
  "slitPupilRadius" : 71,

  "eyelidIndex"     : "0x00",
  "pupilColor"      : [ 0, 0, 0 ],
  "pupilMin"        : 0.05,
  "pupilMax"        : 0.25,
  "backColor"       : [ 80, 0, 0 ],

  "irisTexture"     : "/demon/iris.bmp",
  "scleraTexture"   : "/demon/sclera.bmp",
  "upperEyelid"     : "/demon/upper.bmp",
  "lowerEyelid"     : "/demon/lower.bmp",

  "tracking"        : false,

  // Per-eye overrides. This build reads the block named by EYE_SIDE in
  // eye_config.h, which defaults to "left".
  "left" : {
    "irisSpin"      : -18
  },
  "right" : {
    "irisSpin"      : 18
  }
}
