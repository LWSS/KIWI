#pragma once
// Shared material render-state names and bit masks.
const MtlStateMapBitName s_alphaTestBitNames[5] =
{
    { "Always", 0x800 },
    { "GE128", 0x3000 },
    { "GT0", 0x1000 },
    { "LT128", 0x2000 },
    { 0 }
};

const MtlStateMapBitName s_blendOpRgbBitNames[7] =
{
    { "Disable", 0 },
    { "Add", 0x100  },
    { "Subtract", 0x200 },
    { "RevSubtract", 0x300 },
    { "Min", 0x400 },
    { "Max", 0x500 },
    {0}
};

const MtlStateMapBitName s_srcBlendRgbBitNames[11] =
{
    { "Zero", 1 },
    { "One", 2 },
    { "SrcColor", 3 },
    { "InvSrcColor", 4 },
    { "SrcAlpha", 5 },
    { "InvSrcAlpha", 6 },
    { "DestAlpha", 7 },
    { "InvDestAlpha", 8 },
    { "DestColor", 9 },
    { "InvDestColor", 10 },
    {0}
};

const MtlStateMapBitName s_dstBlendRgbBitNames[11] =
{
    { "Zero", 0x10 },
    { "One", 0x20 },
    { "SrcColor", 0x30 },
    { "InvSrcColor", 0x40 },
    { "SrcAlpha", 0x50 },
    { "InvSrcAlpha", 0x60 },
    { "DestAlpha", 0x70 },
    { "InvDestAlpha", 0x80 },
    { "DestColor", 0x90 },
    { "InvDestColor", 0xA0 },
    {0}
};

const MtlStateMapBitName s_blendOpAlphaBitNames[7] =
{
    { "Disable", 0 },
    { "Add", 0x1000000  },
    { "Subtract", 0x2000000 },
    { "RevSubtract", 0x3000000 },
    { "Min", 0x4000000 },
    { "Max", 0x5000000 },
    {0}
};

const MtlStateMapBitName s_srcBlendAlphaBitNames[11] =
{
    { "Zero",           0x10000 },
    { "One",            0x20000 },
    { "SrcColor",       0x30000 },
    { "InvSrcColor",    0x40000 },
    { "SrcAlpha",       0x50000 },
    { "InvSrcAlpha",    0x60000 },
    { "DestAlpha",      0x70000 },
    { "InvDestAlpha",   0x80000 },
    { "DestColor",      0x90000 },
    { "InvDestColor",   0xA0000 },
    {0}
};

const MtlStateMapBitName s_dstBlendAlphaBitNames[11] =
{
    { "Zero",           0x100000 },
    { "One",            0x200000 },
    { "SrcColor",       0x300000 },
    { "InvSrcColor",    0x400000 },
    { "SrcAlpha",       0x500000 },
    { "InvSrcAlpha",    0x600000 },
    { "DestAlpha",      0x700000 },
    { "InvDestAlpha",   0x800000 },
    { "DestColor",      0x900000 },
    { "InvDestColor",   0xA00000 },
    {0}
};

const MtlStateMapBitName s_cullFaceBitNames[4] =
{
    { "None", 0x4000 },
    { "Back", 0x8000 },
    { "Front",0xC000 },
    {0}
};

const MtlStateMapBitName s_colorWriteRgbBitNames[3] =
{
    { "Enable", 0x8000000 },
    { "Disable", 0 },
    {0}
};

const MtlStateMapBitName s_colorWriteAlphaBitNames[3] =
{
    { "Enable", 0x10000000 },
    { "Disable", 0 },
    {0}
};

const MtlStateMapBitName s_depthTestBitNames[6] =
{
    { "Disable", 2 },
    { "Less", 4 },
    { "LessEqual", 0xC },
    { "Equal", 8 },
    { "Always", 0 },
    { 0 }
};

const MtlStateMapBitName s_depthWriteBitNames[3] =
{
    { "Enable", 1 },
    { "Disable", 0 },
    {0}
};

const MtlStateMapBitName s_polygonOffsetBitNames[5] =
{
    { "0", 0 },
    { "1", 0x10 },
    { "2", 0x20 },
    { "shadowmap", 0x30 },
    {0}
};

const MtlStateMapBitName s_stencilBitNames[4] =
{
    { "Disable", 0 },
    { "OneSided", 0x40 },
    { "TwoSided", 0xC0 },
    {0}
};

const MtlStateMapBitName s_stencilOpFrontPassBitNames[9] =
{
    { "Keep", 0 },
    { "Zero", 0x100 },
    { "Replace", 0x200 },
    { "IncrSat", 0x300 },
    { "DecrSat", 0x400 },
    { "Invert", 0x500 },
    { "Incr", 0x600 },
    { "Decr", 0x700 },
    { 0 }
};

const MtlStateMapBitName s_stencilFuncFrontBitNames[9] =
{
    { "Never",          0 },
    { "Less",           0x20000 },
    { "Equal",          0x40000 },
    { "LessEqual",      0x60000 },
    { "Greater",        0x80000 },
    { "NotEqual",       0xA0000 },
    { "GreaterEqual",   0xC0000 },
    { "Always",         0xE0000 },
    {0}
};

const MtlStateMapBitName s_stencilOpFrontFailBitNames[9] =
{
    { "Keep", 0 },
    { "Zero", 0x800 },
    { "Replace", 0x1000 },
    { "IncrSat", 0x1800 },
    { "DecrSat", 0x2000 },
    { "Invert", 0x2800 },
    { "Incr", 0x3000 },
    { "Decr", 0x3800 },
    {0}
};

const MtlStateMapBitName s_stencilOpFrontZFailBitNames[9] =
{
    { "Keep",    0 },
    { "Zero",   0x4000 },
    { "Replace", 0x8000 },
    { "IncrSat", 0xC000 },
    { "DecrSat", 0x10000 },
    { "Invert",  0x14000 },
    { "Incr",    0x18000 },
    { "Decr",    0x1C000 },
    {0}
};

const MtlStateMapBitName s_stencilFuncBackBitNames[9] =
{
    { "Never", 0 },
    { "Less", 0x20000000 },
    { "Equal", 0x40000000 },
    { "LessEqual", 0x60000000 },
    { "Greater", 0x80000000 },
    { "NotEqual", 0x0A0000000 },
    { "GreaterEqual", 0x0C0000000 },
    { "Always", 0x0E0000000 },
    {0}
};

const MtlStateMapBitName s_stencilOpBackPassBitNames[9] =
{
    { "Keep", 0 },
    { "Zero",       0x100000 },
    { "Replace",    0x200000 },
    { "IncrSat",    0x300000 },
    { "DecrSat",    0x400000 },
    { "Invert",     0x500000 },
    { "Incr",       0x600000 },
    { "Decr",       0x700000 },
    {0}
};

const MtlStateMapBitName s_stencilOpBackFailBitNames[9] =
{
    { "Keep",       0        },
    { "Zero",       0x800000 },
    { "Replace",    0x1000000 },
    { "IncrSat",    0x1800000 },
    { "DecrSat",    0x2000000 },
    { "Invert",     0x2800000 },
    { "Incr",       0x3000000 },
    { "Decr",       0x3800000 },
    {0}
};

const MtlStateMapBitName s_stencilOpBackZFailBitNames[9] =
{
    { "Keep",       0        },
    { "Zero",       0x4000000 },
    { "Replace",    0x8000000 },
    { "IncrSat",    0xC000000 },
    { "DecrSat",    0x10000000 },
    { "Invert",     0x14000000 },
    { "Incr",       0x18000000 },
    { "Decr",       0x1C000000 },
    {0}
};

const MtlStateMapBitName s_wireframeBitNames[3] =
{
    { "Enable", 0x80000000 },
    { "Disable", 0 },
    {0}
};

const MtlStateMapBitGroup s_stateMapSrcBitGroup[23] =
{
  { "mtlAlphaTest", s_alphaTestBitNames, { 14336, 0 } },
  { "mtlBlendOp", s_blendOpRgbBitNames, { 1792, 0 } },
  { "mtlSrcBlend", s_srcBlendRgbBitNames, { 15, 0 } },
  { "mtlDestBlend", s_dstBlendRgbBitNames, { 240, 0 } },
  { "mtlBlendOpAlpha", s_blendOpAlphaBitNames, { 117440512, 0 } },
  { "mtlSrcBlendAlpha", s_srcBlendAlphaBitNames, { 983040, 0 } },
  { "mtlDestBlendAlpha", s_dstBlendAlphaBitNames, { 15728640, 0 } },
  { "mtlCullFace", s_cullFaceBitNames, { 49152, 0 } },
  { "mtlColorWriteRgb", s_colorWriteRgbBitNames, { 134217728, 0 } },
  { "mtlColorWriteAlpha", s_colorWriteAlphaBitNames, { 268435456, 0 } },
  { "mtlDepthTest", s_depthTestBitNames, { 0, 14 } },
  { "mtlDepthWrite", s_depthWriteBitNames, { 0, 1 } },
  { "mtlPolygonOffset", s_polygonOffsetBitNames, { 0, 48 } },
  { "mtlStencil", s_stencilBitNames, { 0, 192 } },
  { "mtlStencilFuncFront", s_stencilFuncFrontBitNames, { 0, 917504 } },
  { "mtlStencilOpFrontPass", s_stencilOpFrontPassBitNames, { 0, 1792 } },
  { "mtlStencilOpFrontFail", s_stencilOpFrontFailBitNames, { 0, 14336 } },
  { "mtlStencilOpFrontZFail", s_stencilOpFrontZFailBitNames, { 0, 114688 } },
  { "mtlStencilFuncBack", s_stencilFuncBackBitNames, { 0, 917504 } },
  { "mtlStencilOpBackPass", s_stencilOpBackPassBitNames, { 0, 1792 } },
  { "mtlStencilOpBackFail", s_stencilOpBackFailBitNames, { 0, 14336 } },
  { "mtlStencilOpBackZFail", s_stencilOpBackZFailBitNames, { 0, 114688 } },
  { NULL, NULL, { 0, 0 } }
}; // idb
const MtlStateMapBitGroup s_stateMapDstAlphaTestBitGroup[2] =
{
  { "alphaTest", s_alphaTestBitNames, { 14336, 0 } },
  { NULL, NULL, { 0, 0 } }
}; // idb
const MtlStateMapBitGroup s_stateMapDstBlendFuncRgbBitGroup[4] =
{
  { "blendFuncRgb", s_blendOpRgbBitNames, { 1792, 0 } },
  { "blendFuncRgb", s_srcBlendRgbBitNames, { 1807, 0 } },
  { "blendFuncRgb", s_dstBlendRgbBitNames, { 2032, 0 } },
  { NULL, NULL, { 0, 0 } }
}; // idb
const MtlStateMapBitGroup s_stateMapDstBlendFuncAlphaBitGroup[4] =
{
  { "blendFuncAlpha", s_blendOpAlphaBitNames, { 117440512, 0 } },
  { "blendFuncAlpha", s_srcBlendAlphaBitNames, { 983040, 0 } },
  { "blendFuncAlpha", s_dstBlendAlphaBitNames, { 15728640, 0 } },
  { NULL, NULL, { 0, 0 } }
}; // idb
const MtlStateMapBitGroup s_stateMapDstCullFaceBitGroup[2] = { { "cullFace", s_cullFaceBitNames, { 49152, 0 } }, { NULL, NULL, { 0, 0 } } }; // idb
const MtlStateMapBitGroup s_stateMapDstDepthTestBitGroup[2] = { { "depthTest", s_depthTestBitNames, { 0, 14 } }, { NULL, NULL, { 0, 0 } } }; // idb
const MtlStateMapBitGroup s_stateMapDstDepthWriteBitGroup[2] = { { "depthWrite", s_depthWriteBitNames, { 0, 1 } }, { NULL, NULL, { 0, 0 } } }; // idb
const MtlStateMapBitGroup s_stateMapDstColorWriteBitGroup[3] =
{
  { "colorWrite", s_colorWriteRgbBitNames, { 134217728, 0 } },
  { "colorWrite", s_colorWriteAlphaBitNames, { 268435456, 0 } },
  { NULL, NULL, { 0, 0 } }
}; // idb
const MtlStateMapBitGroup s_stateMapDstPolygonOffsetBitGroup[2] =
{
  { "polygonOffset", s_polygonOffsetBitNames, { 0, 48 } },
  { NULL, NULL, { 0, 0 } }
}; // idb
const MtlStateMapBitGroup s_stateMapDstWireframeBitGroup[2] =
{
  { "wireframe", s_wireframeBitNames, { 2147483648, 0 } },
  { NULL, NULL, { 0, 0 } }
}; // idb
const MtlStateMapBitGroup s_stateMapDstStencilBitGroup[10] =
{
  { "stencil", s_stencilBitNames, { 0, 192 } },
  { "stencil", s_stencilFuncFrontBitNames, { 0, 917504 } },
  { "stencil", s_stencilOpFrontPassBitNames, { 0, 1792 } },
  { "stencil", s_stencilOpFrontFailBitNames, { 0, 14336 } },
  { "stencil", s_stencilOpFrontZFailBitNames, { 0, 114688 } },
  { "stencil", s_stencilFuncBackBitNames, { 0, 917504 } },
  { "stencil", s_stencilOpBackPassBitNames, { 0, 1792 } },
  { "stencil", s_stencilOpBackFailBitNames, { 0, 14336 } },
  { "stencil", s_stencilOpBackZFailBitNames, { 0, 114688 } },
  { NULL, NULL, { 0, 0 } }
}; // idb

