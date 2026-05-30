param(
    [string] $_shaderc = "../../../../libs/bgfx/release/bin/shaderc.exe",
    [string] $_bgfxSource = "../../../../libs/bgfx/source"
)

$shaderc = Join-Path $PSScriptRoot $_shaderc
$bgfxSource = Join-Path $PSScriptRoot $_bgfxSource
$bgfxSourceSrc = Join-Path $bgfxSource "src"

$vert = Join-Path $PSScriptRoot "embedded.vert"
$vert_dest = Join-Path $PSScriptRoot "embedded.vert_"

$frag = Join-Path $PSScriptRoot "embedded.frag"
$frag_dest = Join-Path $PSScriptRoot "embedded.frag_"

$varying = Join-Path $PSScriptRoot "varying.def.sc"

& $shaderc  --verbose -f $vert -i $bgfxSourceSrc -o "${vert_dest}dx11.h" --bin2c "embedded_vert_dx11" --type vertex -p s_5_0 -O 3
& $shaderc  --verbose -f $vert -i $bgfxSourceSrc -o "${vert_dest}dx12.h" --bin2c "embedded_vert_dx12" --type vertex -p s_6_9 -O 3
& $shaderc  --verbose -f $vert -i $bgfxSourceSrc -o "${vert_dest}opengl.h" --bin2c "embedded_vert_opengl" --type vertex -p 440
& $shaderc  --verbose -f $vert -i $bgfxSourceSrc -o "${vert_dest}vulkan.h" --bin2c "embedded_vert_vulkan" --type vertex -p spirv16-13
& $shaderc  --verbose -f $vert -i $bgfxSourceSrc -o "${vert_dest}metal.h" --bin2c "embedded_vert_metal" --type vertex -p metal31-14 --platform osx
& $shaderc  --verbose -f $vert -i $bgfxSourceSrc -o "${vert_dest}opengles.h" --bin2c "embedded_vert_opengles" --type vertex -p 320_es

& $shaderc  --verbose -f $frag -i $bgfxSourceSrc -o "${frag_dest}dx11.h" --bin2c "embedded_frag_dx11" --type fragment -p s_5_0 -O 3
& $shaderc  --verbose -f $frag -i $bgfxSourceSrc -o "${frag_dest}dx12.h" --bin2c "embedded_frag_dx12" --type fragment -p s_6_9 -O 3
& $shaderc  --verbose -f $frag -i $bgfxSourceSrc -o "${frag_dest}opengl.h" --bin2c "embedded_frag_opengl" --type fragment -p 440
& $shaderc  --verbose -f $frag -i $bgfxSourceSrc -o "${frag_dest}vulkan.h" --bin2c "embedded_frag_vulkan" --type fragment -p spirv16-13
& $shaderc  --verbose -f $frag -i $bgfxSourceSrc -o "${frag_dest}metal.h" --bin2c "embedded_frag_metal" --type fragment -p metal31-14 --platform osx
& $shaderc  --verbose -f $frag -i $bgfxSourceSrc -o "${frag_dest}opengles.h" --bin2c "embedded_frag_opengles" --type fragment -p 320_es