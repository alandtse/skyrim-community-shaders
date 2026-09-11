struct VS_INPUT
{
	float4 Position: POSITION0;
	float2 TexCoord: TEXCOORD0;
	float4 Normal: NORMAL0;
	float4 Color: COLOR0;
	float4 InstanceData1: TEXCOORD4;
	float4 InstanceData2: TEXCOORD5;
	float4 InstanceData3: TEXCOORD6;
	float4 InstanceData4: TEXCOORD7;
	uint InstanceID: SV_InstanceID;
};

float4 main(VS_INPUT input) : SV_Position
{
	return input.Position + input.Normal + input.Color +
	       input.InstanceData1 + input.InstanceData2 + input.InstanceData3 + input.InstanceData4;
}
