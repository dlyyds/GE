#type vertex
struct VSInput {
    float3 position : POSITION;  // 语义：顶点位置
    float4 color    : COLOR;     // 语义：顶点颜色
};
struct VSOutput {
    float4 position : SV_POSITION;  // 系统语义：裁剪空间位置
    float4 color    : COLOR;
};
VSOutput main(VSInput input) {
    VSOutput output;
    output.position = float4(input.position, 1.0f);  // 转为齐次坐标
    output.color    = input.color;
    return output;
}


#type fragment
struct PSInput {
    float4 position : SV_POSITION;
    float4 color    : COLOR;
};
float4 main(PSInput input) : SV_TARGET {  // SV_TARGET：输出到渲染目标
    return input.color;
}