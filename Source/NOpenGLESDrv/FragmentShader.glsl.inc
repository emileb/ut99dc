R""(

varying mediump vec3 vPosition;
#ifdef SF_Texture0
varying mediump vec2 vTexCoord0;
#endif
#ifdef SF_Texture1
varying mediump vec2 vTexCoord1;
#endif
#ifdef SF_Texture2
varying mediump vec2 vTexCoord2;
#endif
#ifdef SF_Texture3
varying mediump vec2 vTexCoord3;
#endif
#ifdef SF_VtxColor
varying lowp vec4 vVtxColor;
#endif
#ifdef SF_VtxFog
varying lowp vec4 vVtxFog;
#endif

uniform highp float uBrightness;

#ifdef SF_Texture0
uniform sampler2D uTexture0;
#endif
#ifdef SF_Texture1
uniform sampler2D uTexture1;
#endif
#ifdef SF_Texture2
uniform sampler2D uTexture2;
#endif
#ifdef SF_Texture3
uniform sampler2D uTexture3;
#endif

#ifndef LIGHTMAP_OVERBRIGHT
#define LIGHTMAP_OVERBRIGHT 1.0
#endif

void main()
{
	lowp vec4 Out = vec4(1.0);

#ifdef SF_Texture0
	lowp vec4 Texel0 = texture2D(uTexture0, vTexCoord0);
	Out = Texel0;
#endif

#ifdef SF_AlphaTest
	if( Out.a < 0.5 )
		discard;
#endif

#ifdef SF_Texture1
	lowp vec4 Texel1 = texture2D(uTexture1, vTexCoord1);
#ifdef SF_Lightmap
	Out.rgb *= Texel1.rgb * 2.0 * LIGHTMAP_OVERBRIGHT;
#else
	Out *= Texel1;
#endif
#endif

#ifdef SF_Texture2
	lowp vec4 Texel2 = texture2D(uTexture2, vTexCoord2);
#ifdef SF_Fogmap
	Texel2 *= 2.0;
	Out.rgb = Texel2.rgb + Out.rgb * (1.0 - Texel2.a);
	Out.a = Texel2.a;
#else
	Out *= Texel2;
#endif
#endif

#ifdef SF_Texture3
	lowp vec4 Texel3 = texture2D(uTexture3, vTexCoord3) * 2.0;
#ifdef SF_Detail
	// TODO: XOpenGLDrv does some RGB->HSV->RGB magic on detail maps, we just * 2 instead
	lowp float DetailScale = clamp(0.65 - vPosition.z / 380.0, 0.0, 1.0);
	Texel3 = mix(vec4(1.0), Texel3, DetailScale);
	Out.rgb *= Texel3.rgb;
#else
	Out.rgb *= Texel3.rgb;
#endif
#endif

#ifdef SF_VtxColor
	Out *= vVtxColor;
#endif

#ifdef SF_VtxFog
	Out.rgb = vVtxFog.rgb + Out.rgb * (1.0 - vVtxFog.a);
#endif

#ifndef SF_Modulated
	// True gamma ramp, same value desktop feeds SDL_SetWindowBrightness() (a pow
	// ramp, not a linear scale) - a linear multiply can never lift near-black
	// pixels, leaving shadowed areas pitch black. Same fix as UE1.
	//
	// Skipped for PF_Modulated draws (decals, shadow blobs): those rely on
	// glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR) - a plain 2x multiply against
	// whatever is already in the framebuffer (itself already gamma-corrected
	// from its own draw). Their "invisible" neutral-gray fill (0.5) only
	// cancels out (2*0.5*Dst == Dst) if it reaches the blend stage
	// unmodified; re-applying this curve to it independently breaks that
	// cancellation (pow(0.5, 1/Gamma) != 0.5 for Gamma != 1), which showed up
	// as a visible light-grey square around every decal.
	highp float Gamma = 0.5 + 1.5 * uBrightness;
	Out.rgb = pow( max( Out.rgb, 0.0 ), vec3( 1.0 / Gamma ) );
#endif

	gl_FragColor = Out;
}

)""
