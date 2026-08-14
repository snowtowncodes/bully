#!/usr/bin/env python3
"""Generate the exact IDirect3DDevice9 method block used by proxy.cpp.

The method list mirrors the Windows SDK IDirect3DDevice9 declaration. The
first three vtable slots (IUnknown) are implemented by ProxyIDirect3DDevice9;
this generator emits the remaining 116 slots in order.
"""

from pathlib import Path


# (method name, declaration after STDMETHOD/STDMETHOD_, argument names)
METHODS = (
    ("TestCooperativeLevel", "STDMETHOD(TestCooperativeLevel)()", ""),
    ("GetAvailableTextureMem", "STDMETHOD_(UINT, GetAvailableTextureMem)()", ""),
    ("EvictManagedResources", "STDMETHOD(EvictManagedResources)()", ""),
    ("GetDirect3D", "STDMETHOD(GetDirect3D)(IDirect3D9** ppD3D9)", "ppD3D9"),
    ("GetDeviceCaps", "STDMETHOD(GetDeviceCaps)(D3DCAPS9* pCaps)", "pCaps"),
    ("GetDisplayMode", "STDMETHOD(GetDisplayMode)(UINT iSwapChain,D3DDISPLAYMODE* pMode)", "iSwapChain, pMode"),
    ("GetCreationParameters", "STDMETHOD(GetCreationParameters)(D3DDEVICE_CREATION_PARAMETERS* pParameters)", "pParameters"),
    ("SetCursorProperties", "STDMETHOD(SetCursorProperties)(UINT XHotSpot,UINT YHotSpot,IDirect3DSurface9* pCursorBitmap)", "XHotSpot, YHotSpot, pCursorBitmap"),
    ("SetCursorPosition", "STDMETHOD_(void, SetCursorPosition)(int X,int Y,DWORD Flags)", "X, Y, Flags"),
    ("ShowCursor", "STDMETHOD_(BOOL, ShowCursor)(BOOL bShow)", "bShow"),
    ("CreateAdditionalSwapChain", "STDMETHOD(CreateAdditionalSwapChain)(D3DPRESENT_PARAMETERS* pPresentationParameters,IDirect3DSwapChain9** pSwapChain)", "pPresentationParameters, pSwapChain"),
    ("GetSwapChain", "STDMETHOD(GetSwapChain)(UINT iSwapChain,IDirect3DSwapChain9** pSwapChain)", "iSwapChain, pSwapChain"),
    ("GetNumberOfSwapChains", "STDMETHOD_(UINT, GetNumberOfSwapChains)()", ""),
    ("Reset", "STDMETHOD(Reset)(D3DPRESENT_PARAMETERS* pPresentationParameters)", "pPresentationParameters"),
    ("Present", "STDMETHOD(Present)(CONST RECT* pSourceRect,CONST RECT* pDestRect,HWND hDestWindowOverride,CONST RGNDATA* pDirtyRegion)", "pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion"),
    ("GetBackBuffer", "STDMETHOD(GetBackBuffer)(UINT iSwapChain,UINT iBackBuffer,D3DBACKBUFFER_TYPE Type,IDirect3DSurface9** ppBackBuffer)", "iSwapChain, iBackBuffer, Type, ppBackBuffer"),
    ("GetRasterStatus", "STDMETHOD(GetRasterStatus)(UINT iSwapChain,D3DRASTER_STATUS* pRasterStatus)", "iSwapChain, pRasterStatus"),
    ("SetDialogBoxMode", "STDMETHOD(SetDialogBoxMode)(BOOL bEnableDialogs)", "bEnableDialogs"),
    ("SetGammaRamp", "STDMETHOD_(void, SetGammaRamp)(UINT iSwapChain,DWORD Flags,CONST D3DGAMMARAMP* pRamp)", "iSwapChain, Flags, pRamp"),
    ("GetGammaRamp", "STDMETHOD_(void, GetGammaRamp)(UINT iSwapChain,D3DGAMMARAMP* pRamp)", "iSwapChain, pRamp"),
    ("CreateTexture", "STDMETHOD(CreateTexture)(UINT Width,UINT Height,UINT Levels,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DTexture9** ppTexture,HANDLE* pSharedHandle)", "Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle"),
    ("CreateVolumeTexture", "STDMETHOD(CreateVolumeTexture)(UINT Width,UINT Height,UINT Depth,UINT Levels,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DVolumeTexture9** ppVolumeTexture,HANDLE* pSharedHandle)", "Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture, pSharedHandle"),
    ("CreateCubeTexture", "STDMETHOD(CreateCubeTexture)(UINT EdgeLength,UINT Levels,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DCubeTexture9** ppCubeTexture,HANDLE* pSharedHandle)", "EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle"),
    ("CreateVertexBuffer", "STDMETHOD(CreateVertexBuffer)(UINT Length,DWORD Usage,DWORD FVF,D3DPOOL Pool,IDirect3DVertexBuffer9** ppVertexBuffer,HANDLE* pSharedHandle)", "Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle"),
    ("CreateIndexBuffer", "STDMETHOD(CreateIndexBuffer)(UINT Length,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DIndexBuffer9** ppIndexBuffer,HANDLE* pSharedHandle)", "Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle"),
    ("CreateRenderTarget", "STDMETHOD(CreateRenderTarget)(UINT Width,UINT Height,D3DFORMAT Format,D3DMULTISAMPLE_TYPE MultiSample,DWORD MultisampleQuality,BOOL Lockable,IDirect3DSurface9** ppSurface,HANDLE* pSharedHandle)", "Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle"),
    ("CreateDepthStencilSurface", "STDMETHOD(CreateDepthStencilSurface)(UINT Width,UINT Height,D3DFORMAT Format,D3DMULTISAMPLE_TYPE MultiSample,DWORD MultisampleQuality,BOOL Discard,IDirect3DSurface9** ppSurface,HANDLE* pSharedHandle)", "Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle"),
    ("UpdateSurface", "STDMETHOD(UpdateSurface)(IDirect3DSurface9* pSourceSurface,CONST RECT* pSourceRect,IDirect3DSurface9* pDestinationSurface,CONST POINT* pDestPoint)", "pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint"),
    ("UpdateTexture", "STDMETHOD(UpdateTexture)(IDirect3DBaseTexture9* pSourceTexture,IDirect3DBaseTexture9* pDestinationTexture)", "pSourceTexture, pDestinationTexture"),
    ("GetRenderTargetData", "STDMETHOD(GetRenderTargetData)(IDirect3DSurface9* pRenderTarget,IDirect3DSurface9* pDestSurface)", "pRenderTarget, pDestSurface"),
    ("GetFrontBufferData", "STDMETHOD(GetFrontBufferData)(UINT iSwapChain,IDirect3DSurface9* pDestSurface)", "iSwapChain, pDestSurface"),
    ("StretchRect", "STDMETHOD(StretchRect)(IDirect3DSurface9* pSourceSurface,CONST RECT* pSourceRect,IDirect3DSurface9* pDestSurface,CONST RECT* pDestRect,D3DTEXTUREFILTERTYPE Filter)", "pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter"),
    ("ColorFill", "STDMETHOD(ColorFill)(IDirect3DSurface9* pSurface,CONST RECT* pRect,D3DCOLOR color)", "pSurface, pRect, color"),
    ("CreateOffscreenPlainSurface", "STDMETHOD(CreateOffscreenPlainSurface)(UINT Width,UINT Height,D3DFORMAT Format,D3DPOOL Pool,IDirect3DSurface9** ppSurface,HANDLE* pSharedHandle)", "Width, Height, Format, Pool, ppSurface, pSharedHandle"),
    ("SetRenderTarget", "STDMETHOD(SetRenderTarget)(DWORD RenderTargetIndex,IDirect3DSurface9* pRenderTarget)", "RenderTargetIndex, pRenderTarget"),
    ("GetRenderTarget", "STDMETHOD(GetRenderTarget)(DWORD RenderTargetIndex,IDirect3DSurface9** ppRenderTarget)", "RenderTargetIndex, ppRenderTarget"),
    ("SetDepthStencilSurface", "STDMETHOD(SetDepthStencilSurface)(IDirect3DSurface9* pNewZStencil)", "pNewZStencil"),
    ("GetDepthStencilSurface", "STDMETHOD(GetDepthStencilSurface)(IDirect3DSurface9** ppZStencilSurface)", "ppZStencilSurface"),
    ("BeginScene", "STDMETHOD(BeginScene)()", ""),
    ("EndScene", "STDMETHOD(EndScene)()", ""),
    ("Clear", "STDMETHOD(Clear)(DWORD Count,CONST D3DRECT* pRects,DWORD Flags,D3DCOLOR Color,float Z,DWORD Stencil)", "Count, pRects, Flags, Color, Z, Stencil"),
    ("SetTransform", "STDMETHOD(SetTransform)(D3DTRANSFORMSTATETYPE State,CONST D3DMATRIX* pMatrix)", "State, pMatrix"),
    ("GetTransform", "STDMETHOD(GetTransform)(D3DTRANSFORMSTATETYPE State,D3DMATRIX* pMatrix)", "State, pMatrix"),
    ("MultiplyTransform", "STDMETHOD(MultiplyTransform)(D3DTRANSFORMSTATETYPE State,CONST D3DMATRIX* pMatrix)", "State, pMatrix"),
    ("SetViewport", "STDMETHOD(SetViewport)(CONST D3DVIEWPORT9* pViewport)", "pViewport"),
    ("GetViewport", "STDMETHOD(GetViewport)(D3DVIEWPORT9* pViewport)", "pViewport"),
    ("SetMaterial", "STDMETHOD(SetMaterial)(CONST D3DMATERIAL9* pMaterial)", "pMaterial"),
    ("GetMaterial", "STDMETHOD(GetMaterial)(D3DMATERIAL9* pMaterial)", "pMaterial"),
    ("SetLight", "STDMETHOD(SetLight)(DWORD Index,CONST D3DLIGHT9* pLight)", "Index, pLight"),
    ("GetLight", "STDMETHOD(GetLight)(DWORD Index,D3DLIGHT9* pLight)", "Index, pLight"),
    ("LightEnable", "STDMETHOD(LightEnable)(DWORD Index,BOOL Enable)", "Index, Enable"),
    ("GetLightEnable", "STDMETHOD(GetLightEnable)(DWORD Index,BOOL* pEnable)", "Index, pEnable"),
    ("SetClipPlane", "STDMETHOD(SetClipPlane)(DWORD Index,CONST float* pPlane)", "Index, pPlane"),
    ("GetClipPlane", "STDMETHOD(GetClipPlane)(DWORD Index,float* pPlane)", "Index, pPlane"),
    ("SetRenderState", "STDMETHOD(SetRenderState)(D3DRENDERSTATETYPE State,DWORD Value)", "State, Value"),
    ("GetRenderState", "STDMETHOD(GetRenderState)(D3DRENDERSTATETYPE State,DWORD* pValue)", "State, pValue"),
    ("CreateStateBlock", "STDMETHOD(CreateStateBlock)(D3DSTATEBLOCKTYPE Type,IDirect3DStateBlock9** ppSB)", "Type, ppSB"),
    ("BeginStateBlock", "STDMETHOD(BeginStateBlock)()", ""),
    ("EndStateBlock", "STDMETHOD(EndStateBlock)(IDirect3DStateBlock9** ppSB)", "ppSB"),
    ("SetClipStatus", "STDMETHOD(SetClipStatus)(CONST D3DCLIPSTATUS9* pClipStatus)", "pClipStatus"),
    ("GetClipStatus", "STDMETHOD(GetClipStatus)(D3DCLIPSTATUS9* pClipStatus)", "pClipStatus"),
    ("GetTexture", "STDMETHOD(GetTexture)(DWORD Stage,IDirect3DBaseTexture9** ppTexture)", "Stage, ppTexture"),
    ("SetTexture", "STDMETHOD(SetTexture)(DWORD Stage,IDirect3DBaseTexture9* pTexture)", "Stage, pTexture"),
    ("GetTextureStageState", "STDMETHOD(GetTextureStageState)(DWORD Stage,D3DTEXTURESTAGESTATETYPE Type,DWORD* pValue)", "Stage, Type, pValue"),
    ("SetTextureStageState", "STDMETHOD(SetTextureStageState)(DWORD Stage,D3DTEXTURESTAGESTATETYPE Type,DWORD Value)", "Stage, Type, Value"),
    ("GetSamplerState", "STDMETHOD(GetSamplerState)(DWORD Sampler,D3DSAMPLERSTATETYPE Type,DWORD* pValue)", "Sampler, Type, pValue"),
    ("SetSamplerState", "STDMETHOD(SetSamplerState)(DWORD Sampler,D3DSAMPLERSTATETYPE Type,DWORD Value)", "Sampler, Type, Value"),
    ("ValidateDevice", "STDMETHOD(ValidateDevice)(DWORD* pNumPasses)", "pNumPasses"),
    ("SetPaletteEntries", "STDMETHOD(SetPaletteEntries)(UINT PaletteNumber,CONST PALETTEENTRY* pEntries)", "PaletteNumber, pEntries"),
    ("GetPaletteEntries", "STDMETHOD(GetPaletteEntries)(UINT PaletteNumber,PALETTEENTRY* pEntries)", "PaletteNumber, pEntries"),
    ("SetCurrentTexturePalette", "STDMETHOD(SetCurrentTexturePalette)(UINT PaletteNumber)", "PaletteNumber"),
    ("GetCurrentTexturePalette", "STDMETHOD(GetCurrentTexturePalette)(UINT* PaletteNumber)", "PaletteNumber"),
    ("SetScissorRect", "STDMETHOD(SetScissorRect)(CONST RECT* pRect)", "pRect"),
    ("GetScissorRect", "STDMETHOD(GetScissorRect)(RECT* pRect)", "pRect"),
    ("SetSoftwareVertexProcessing", "STDMETHOD(SetSoftwareVertexProcessing)(BOOL bSoftware)", "bSoftware"),
    ("GetSoftwareVertexProcessing", "STDMETHOD_(BOOL, GetSoftwareVertexProcessing)()", ""),
    ("SetNPatchMode", "STDMETHOD(SetNPatchMode)(float nSegments)", "nSegments"),
    ("GetNPatchMode", "STDMETHOD_(float, GetNPatchMode)()", ""),
    ("DrawPrimitive", "STDMETHOD(DrawPrimitive)(D3DPRIMITIVETYPE PrimitiveType,UINT StartVertex,UINT PrimitiveCount)", "PrimitiveType, StartVertex, PrimitiveCount"),
    ("DrawIndexedPrimitive", "STDMETHOD(DrawIndexedPrimitive)(D3DPRIMITIVETYPE PrimitiveType,INT BaseVertexIndex,UINT MinVertexIndex,UINT NumVertices,UINT startIndex,UINT primCount)", "PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount"),
    ("DrawPrimitiveUP", "STDMETHOD(DrawPrimitiveUP)(D3DPRIMITIVETYPE PrimitiveType,UINT PrimitiveCount,CONST void* pVertexStreamZeroData,UINT VertexStreamZeroStride)", "PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride"),
    ("DrawIndexedPrimitiveUP", "STDMETHOD(DrawIndexedPrimitiveUP)(D3DPRIMITIVETYPE PrimitiveType,UINT MinVertexIndex,UINT NumVertices,UINT PrimitiveCount,CONST void* pIndexData,D3DFORMAT IndexDataFormat,CONST void* pVertexStreamZeroData,UINT VertexStreamZeroStride)", "PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride"),
    ("ProcessVertices", "STDMETHOD(ProcessVertices)(UINT SrcStartIndex,UINT DestIndex,UINT VertexCount,IDirect3DVertexBuffer9* pDestBuffer,IDirect3DVertexDeclaration9* pVertexDecl,DWORD Flags)", "SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags"),
    ("CreateVertexDeclaration", "STDMETHOD(CreateVertexDeclaration)(CONST D3DVERTEXELEMENT9* pVertexElements,IDirect3DVertexDeclaration9** ppDecl)", "pVertexElements, ppDecl"),
    ("SetVertexDeclaration", "STDMETHOD(SetVertexDeclaration)(IDirect3DVertexDeclaration9* pDecl)", "pDecl"),
    ("GetVertexDeclaration", "STDMETHOD(GetVertexDeclaration)(IDirect3DVertexDeclaration9** ppDecl)", "ppDecl"),
    ("SetFVF", "STDMETHOD(SetFVF)(DWORD FVF)", "FVF"),
    ("GetFVF", "STDMETHOD(GetFVF)(DWORD* pFVF)", "pFVF"),
    ("CreateVertexShader", "STDMETHOD(CreateVertexShader)(CONST DWORD* pFunction,IDirect3DVertexShader9** ppShader)", "pFunction, ppShader"),
    ("SetVertexShader", "STDMETHOD(SetVertexShader)(IDirect3DVertexShader9* pShader)", "pShader"),
    ("GetVertexShader", "STDMETHOD(GetVertexShader)(IDirect3DVertexShader9** ppShader)", "ppShader"),
    ("SetVertexShaderConstantF", "STDMETHOD(SetVertexShaderConstantF)(UINT StartRegister,CONST float* pConstantData,UINT Vector4fCount)", "StartRegister, pConstantData, Vector4fCount"),
    ("GetVertexShaderConstantF", "STDMETHOD(GetVertexShaderConstantF)(UINT StartRegister,float* pConstantData,UINT Vector4fCount)", "StartRegister, pConstantData, Vector4fCount"),
    ("SetVertexShaderConstantI", "STDMETHOD(SetVertexShaderConstantI)(UINT StartRegister,CONST int* pConstantData,UINT Vector4iCount)", "StartRegister, pConstantData, Vector4iCount"),
    ("GetVertexShaderConstantI", "STDMETHOD(GetVertexShaderConstantI)(UINT StartRegister,int* pConstantData,UINT Vector4iCount)", "StartRegister, pConstantData, Vector4iCount"),
    ("SetVertexShaderConstantB", "STDMETHOD(SetVertexShaderConstantB)(UINT StartRegister,CONST BOOL* pConstantData,UINT BoolCount)", "StartRegister, pConstantData, BoolCount"),
    ("GetVertexShaderConstantB", "STDMETHOD(GetVertexShaderConstantB)(UINT StartRegister,BOOL* pConstantData,UINT BoolCount)", "StartRegister, pConstantData, BoolCount"),
    ("SetStreamSource", "STDMETHOD(SetStreamSource)(UINT StreamNumber,IDirect3DVertexBuffer9* pStreamData,UINT OffsetInBytes,UINT Stride)", "StreamNumber, pStreamData, OffsetInBytes, Stride"),
    ("GetStreamSource", "STDMETHOD(GetStreamSource)(UINT StreamNumber,IDirect3DVertexBuffer9** ppStreamData,UINT* pOffsetInBytes,UINT* pStride)", "StreamNumber, ppStreamData, pOffsetInBytes, pStride"),
    ("SetStreamSourceFreq", "STDMETHOD(SetStreamSourceFreq)(UINT StreamNumber,UINT Setting)", "StreamNumber, Setting"),
    ("GetStreamSourceFreq", "STDMETHOD(GetStreamSourceFreq)(UINT StreamNumber,UINT* pSetting)", "StreamNumber, pSetting"),
    ("SetIndices", "STDMETHOD(SetIndices)(IDirect3DIndexBuffer9* pIndexData)", "pIndexData"),
    ("GetIndices", "STDMETHOD(GetIndices)(IDirect3DIndexBuffer9** ppIndexData)", "ppIndexData"),
    ("CreatePixelShader", "STDMETHOD(CreatePixelShader)(CONST DWORD* pFunction,IDirect3DPixelShader9** ppShader)", "pFunction, ppShader"),
    ("SetPixelShader", "STDMETHOD(SetPixelShader)(IDirect3DPixelShader9* pShader)", "pShader"),
    ("GetPixelShader", "STDMETHOD(GetPixelShader)(IDirect3DPixelShader9** ppShader)", "ppShader"),
    ("SetPixelShaderConstantF", "STDMETHOD(SetPixelShaderConstantF)(UINT StartRegister,CONST float* pConstantData,UINT Vector4fCount)", "StartRegister, pConstantData, Vector4fCount"),
    ("GetPixelShaderConstantF", "STDMETHOD(GetPixelShaderConstantF)(UINT StartRegister,float* pConstantData,UINT Vector4fCount)", "StartRegister, pConstantData, Vector4fCount"),
    ("SetPixelShaderConstantI", "STDMETHOD(SetPixelShaderConstantI)(UINT StartRegister,CONST int* pConstantData,UINT Vector4iCount)", "StartRegister, pConstantData, Vector4iCount"),
    ("GetPixelShaderConstantI", "STDMETHOD(GetPixelShaderConstantI)(UINT StartRegister,int* pConstantData,UINT Vector4iCount)", "StartRegister, pConstantData, Vector4iCount"),
    ("SetPixelShaderConstantB", "STDMETHOD(SetPixelShaderConstantB)(UINT StartRegister,CONST BOOL* pConstantData,UINT BoolCount)", "StartRegister, pConstantData, BoolCount"),
    ("GetPixelShaderConstantB", "STDMETHOD(GetPixelShaderConstantB)(UINT StartRegister,BOOL* pConstantData,UINT BoolCount)", "StartRegister, pConstantData, BoolCount"),
    ("DrawRectPatch", "STDMETHOD(DrawRectPatch)(UINT Handle,CONST float* pNumSegs,CONST D3DRECTPATCH_INFO* pRectPatchInfo)", "Handle, pNumSegs, pRectPatchInfo"),
    ("DrawTriPatch", "STDMETHOD(DrawTriPatch)(UINT Handle,CONST float* pNumSegs,CONST D3DTRIPATCH_INFO* pTriPatchInfo)", "Handle, pNumSegs, pTriPatchInfo"),
    ("DeletePatch", "STDMETHOD(DeletePatch)(UINT Handle)", "Handle"),
    ("CreateQuery", "STDMETHOD(CreateQuery)(D3DQUERYTYPE Type,IDirect3DQuery9** ppQuery)", "Type, ppQuery"),
)

INTERCEPTED = {
    "TestCooperativeLevel",
    "GetDirect3D",
    "GetDeviceCaps",
    "GetSwapChain",
    "CreateAdditionalSwapChain",
    "Reset",
    "Present",
    "GetFrontBufferData",
    "CreateTexture",
    "CreateRenderTarget",
    "CreateDepthStencilSurface",
    "SetRenderTarget",
    "SetDepthStencilSurface",
    "BeginScene",
    "EndScene",
    "Clear",
    "DrawPrimitive",
    "DrawIndexedPrimitive",
    "DrawPrimitiveUP",
    "DrawIndexedPrimitiveUP",
    "CreateVertexShader",
    "SetVertexShader",
    "CreatePixelShader",
    "SetPixelShader",
}

VOID_METHODS = {"SetCursorPosition", "SetGammaRamp", "GetGammaRamp"}


def main() -> None:
    names = [name for name, _, _ in METHODS]
    assert len(METHODS) == 116, len(METHODS)
    assert len(names) == len(set(names))
    assert INTERCEPTED <= set(names)
    assert not (INTERCEPTED & VOID_METHODS)

    lines = [
        "// Generated by generate_device9_methods.py. Do not edit by hand.",
        "// IDirect3DDevice9 has 119 vtable slots: IUnknown occupies 0-2; this",
        "// file emits slots 3-118 in Windows SDK declaration order.",
        "",
    ]
    for slot, (name, declaration, arguments) in enumerate(METHODS, start=3):
        lines.append(f"    // Vtable slot {slot}: {name}")
        lines.append(f"    {declaration} {{")
        call = f"m_inner->{name}({arguments})"
        if name in INTERCEPTED:
            lines.append(f"        return Intercept_{name}({arguments});")
        elif name in VOID_METHODS:
            lines.append(f"        {call};")
        else:
            lines.append(f"        return {call};")
        lines.append("    }")
        lines.append("")

    output = Path(__file__).with_name("device9_methods.generated.inc")
    output.write_text("\n".join(lines), encoding="ascii")
    print(f"wrote {output} ({len(METHODS)} methods)")


if __name__ == "__main__":
    main()
