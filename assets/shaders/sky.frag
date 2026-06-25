#version 330 core
in vec3 viewRay;
out vec4 FragColor;

uniform float time;
uniform float timeOfDay;
uniform vec3 sunDirection;
uniform vec3 cameraPos;
uniform sampler2D hdriMap;
uniform float windSpeed;
uniform float underwaterFactor;   // 0 = above water, 1 = fully submerged
uniform float weatherExposure;    // storm darkens the whole sky

// Weather-driven cloud appearance (Req 5).
uniform vec3  cloudColor;
uniform float cloudDensity;
uniform float cloudCoverage;

#define PI 3.14159265

// Equirectangular HDRI sampling
vec2 dirToUV(vec3 dir) {
    float phi = atan(dir.z, dir.x);
    float theta = asin(clamp(dir.y, -1.0, 1.0));
    return vec2(phi / (2.0 * PI) + 0.5, theta / PI + 0.5);
}

vec3 sampleHDRI(vec3 dir) {
    return texture(hdriMap, dirToUV(dir)).rgb;
}

// ---- Volumetric clouds (compact march, see clouds.frag for the full
// standalone reference) ----------------------------------------------
float hash13c(vec3 p){ p=fract(p*0.1031); p+=dot(p,p.zyx+31.32); return fract((p.x+p.y)*p.z); }
float vnoise3(vec3 p){
    vec3 i=floor(p); vec3 f=fract(p); f=f*f*(3.0-2.0*f);
    float n000=hash13c(i),n100=hash13c(i+vec3(1,0,0)),n010=hash13c(i+vec3(0,1,0)),n110=hash13c(i+vec3(1,1,0));
    float n001=hash13c(i+vec3(0,0,1)),n101=hash13c(i+vec3(1,0,1)),n011=hash13c(i+vec3(0,1,1)),n111=hash13c(i+vec3(1,1,1));
    return mix(mix(mix(n000,n100,f.x),mix(n010,n110,f.x),f.y),
               mix(mix(n001,n101,f.x),mix(n011,n111,f.x),f.y),f.z);
}
float fbm3c(vec3 p){ float v=0.0,a=0.5; for(int i=0;i<3;i++){v+=a*vnoise3(p);p=p*2.03+vec3(1.7,9.2,3.3);a*=0.5;} return v; } // 3 octaves (was 4): cloud march calls this 48+ x/pixel

// Returns rgb + alpha of clouds for an upward ray.
vec4 marchClouds(vec3 ro, vec3 rd, vec3 sunDir, float coverage, float density) {
    const float B = 120.0, T = 300.0;
    if (rd.y <= 0.03) return vec4(0.0);
    float tN = max((B - ro.y)/rd.y, 0.0);
    float tF = (T - ro.y)/rd.y;
    if (tF <= tN) return vec4(0.0);

    const int STEPS = 32;                 // was 48; dither + early-out hide the banding
    float ss = (tF - tN)/float(STEPS);

    // Dither the start position so the step planes don't line up into
    // visible horizontal bands (the "fringe" artefact in the screenshot).
    float dither = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    vec3 p = ro + rd * (tN + ss * dither);
    vec3 stp = rd*ss;

    // Wind scroll scales with windSpeed → clouds drift faster in wind.
    float wind = time * (0.01 + windSpeed * 0.02);
    vec2 windOff = vec2(wind, wind * 0.6);

    float trans = 1.0; vec3 scat = vec3(0.0);
    vec3 sunCol = vec3(1.0,0.95,0.85), amb = vec3(0.4,0.5,0.65);
    for (int i=0;i<STEPS;i++){
        vec3 q = p*0.0035 + vec3(windOff.x, 0.0, windOff.y);
        float h = (p.y - B)/(T - B);
        float hf = smoothstep(0.0,0.3,h)*smoothstep(1.0,0.55,h);
        // Soft but visible density. Lower threshold => more cloud cover.
        // coverage shifts the threshold so stormy skies fill in.
        float n = fbm3c(q);
        float loThr = mix(0.55, 0.30, clamp(coverage, 0.0, 1.0));
        float d = smoothstep(loThr, loThr + 0.30, n) * hf * density;
        if (d > 0.001){
            float lt = 1.0; vec3 lp = p;
            for(int j=0;j<2;j++){ lp += sunDir*30.0; float hh=(lp.y-B)/(T-B);   // 2 light steps (was 4), longer reach
                float hff=smoothstep(0.0,0.3,hh)*smoothstep(1.0,0.55,hh);
                vec3 lq=lp*0.0035+vec3(windOff.x,0.0,windOff.y);
                float ln=fbm3c(lq);
                float ld=smoothstep(0.45,0.75,ln)*hff*density;
                lt *= exp(-ld*14.0); }
            vec3 lit = amb + sunCol*lt;
            float a = 1.0 - exp(-d*ss*1.4);
            scat += trans*a*lit; trans *= 1.0-a;
            if(trans<0.02) break;
        }
        p += stp;
    }
    return vec4(scat, 1.0 - trans);
}

// 2D hash for stable star grid in spherical UV
float hash21(vec2 p) {
    p = fract(p * vec2(0.1031, 0.1030));
    p += dot(p, p.yx + 33.33);
    return fract((p.x + p.y) * p.x);
}

// Stable star field anchored in world-space sky direction.
// Uses a grid in equirectangular UV so each star has a fixed
// position; the gaussian profile is sampled per-fragment so it
// doesn't pixel-flicker when the camera moves.
vec3 starField(vec3 rd, float t) {
    vec2 uv = dirToUV(rd);
    vec2 grid = uv * vec2(700.0, 350.0);  // density: more cells -> more stars
    vec2 cell = floor(grid);
    vec2 f = fract(grid);

    // Decide if this cell has a star (sparse)
    float r1 = hash21(cell);
    if (r1 < 0.985) return vec3(0.0);

    // Sub-cell position for the star (so they're not centered on a grid)
    vec2 starPos = vec2(hash21(cell + 1.7), hash21(cell + 5.3));
    float dist = length(f - starPos);

    // Soft round profile (gaussian-ish)
    float radius = 0.05 + hash21(cell + 9.1) * 0.06;
    float core = exp(-dist * dist / (radius * radius)) ;

    // Brightness varies between stars
    float bright = 0.6 + hash21(cell + 13.7) * 1.4;

    // Subtle, slow twinkle (not per-frame flicker)
    float tw = 0.85 + 0.15 * sin(t * 0.6 + r1 * 100.0);

    // Slight color variation: bluish to warm
    float colorMix = hash21(cell + 17.3);
    vec3 col = mix(vec3(0.85, 0.92, 1.0), vec3(1.0, 0.93, 0.82), colorMix);

    return col * core * bright * tw;
}

void main() {
    vec3 rd = normalize(viewRay);
    vec3 sunDir = normalize(sunDirection);
    float sunHeight = sunDir.y;
    
    // Calculate sky tints based on sun height
    float dayFactor = smoothstep(-0.1, 0.2, sunHeight);
    float sunsetFactor = smoothstep(-0.2, 0.1, sunHeight) - smoothstep(0.1, 0.4, sunHeight);
    float nightFactor = 1.0 - smoothstep(-0.2, 0.0, sunHeight);
    
    vec3 dayTint = vec3(1.0, 1.0, 1.0);
    vec3 sunsetTint = vec3(1.2, 0.6, 0.3); // Warm sunset/sunrise
    vec3 nightTint = vec3(0.05, 0.1, 0.3); // Moonlight dark blue
    
    vec3 currentTint = dayTint * dayFactor + sunsetTint * sunsetFactor + nightTint * nightFactor;
    float currentExposure = mix(0.1, 1.0, smoothstep(-0.2, 0.2, sunHeight)); // Darken at night
    
    // Below horizon: abyss gradient — full black looking straight down,
    // transitioning to a light greenish-blue toward the horizon. This
    // gives the underwater world a proper "bottomless deep" backdrop.
    if(rd.y < -0.01) {
        // 0 at horizon, 1 straight down.
        float downFactor = clamp(-rd.y, 0.0, 1.0);
        downFactor = pow(downFactor, 0.6);   // bias toward the horizon band

        vec3 abyssDeep   = vec3(0.0, 0.0, 0.0);              // pure black bottom
        vec3 abyssMid    = vec3(0.01, 0.05, 0.07);
        vec3 abyssTop    = vec3(0.05, 0.22, 0.26);          // greenish-blue band

        vec3 abyss = mix(abyssTop, abyssMid, smoothstep(0.0, 0.45, downFactor));
        abyss      = mix(abyss, abyssDeep, smoothstep(0.4, 1.0, downFactor));

        // Blend with a hint of the horizon sky colour just under the line.
        vec3 skyAtHorizon = sampleHDRI(vec3(rd.x, 0.01, rd.z)) * currentTint * currentExposure;
        float horizonBlend = smoothstep(-0.12, 0.0, rd.y);
        abyss = mix(abyss, skyAtHorizon * 0.35, horizonBlend);

        // Fade the HDRI-driven abyss out underwater so the cubemap
        // abyss background (pass 0b) shows through and contributes.
        FragColor = vec4(abyss * currentExposure * weatherExposure, 1.0 - underwaterFactor);
        return;
    }
    
    // Sample HDRI sky and tint it. Clamp to remove the baked-in red sun
    vec3 sky = min(sampleHDRI(rd), vec3(1.2)) * currentTint * currentExposure;
    
    // Procedural Stars — stable in world space, no per-frame flicker
    if (nightFactor > 0.0) {
        float starVisibility = nightFactor * smoothstep(0.0, 0.15, rd.y);
        vec3 starColor = starField(rd, time) * starVisibility;
        // Mask stars where the HDRI sky is bright (clouds / horizon glow)
        float cloudLuma = dot(sky, vec3(0.299, 0.587, 0.114));
        sky += starColor * smoothstep(0.05, 0.0, cloudLuma);
    }
    
    // Add sun disc + bloom on top of HDRI
    // Sun is only visible if it's above horizon (or just slightly below during sunset)
    vec3 sunColor = vec3(0.0);
    if (sunHeight > -0.1) {
        float sunDot = dot(rd, sunDir);
        float sunDisc = smoothstep(0.9995, 0.99985, sunDot);
        sunColor = vec3(1.0, 0.95, 0.85) * 12.0 * sunDisc;
        // Tight glow only — a wide soft bloom here reads as a big white
        // blob filling the screen, which we don't want.
        float sunBloom = pow(max(sunDot, 0.0), 700.0) * 0.6;
        sunColor += vec3(1.0, 0.85, 0.5) * sunBloom;
        
        // Tint sun color during sunset
        sunColor *= mix(vec3(1.0, 0.5, 0.1), vec3(1.0, 1.0, 1.0), smoothstep(-0.1, 0.2, sunHeight));
        sunColor *= smoothstep(-0.1, 0.0, sunHeight); // Fade sun out completely below horizon
    }
    
    // Moon disc
    if (nightFactor > 0.0) {
        vec3 moonDir = normalize(-sunDirection); // Moon is opposite to the sun
        float moonDot = dot(rd, moonDir);
        float moonDisc = smoothstep(0.9995, 0.99985, moonDot);
        vec3 moonCol = vec3(0.8, 0.9, 1.0) * 5.0 * moonDisc;
        float moonBloom = pow(max(moonDot, 0.0), 128.0) * 0.5;
        moonCol += vec3(0.5, 0.6, 1.0) * moonBloom;
        sky += moonCol * nightFactor;
    }
    
    vec3 finalColor = sky + sunColor;

    // ---- Volumetric clouds over the daytime sky ----
    // Fade out at night and near the horizon. Weather drives density,
    // coverage and tint.
    if (dayFactor > 0.01 && rd.y > 0.03) {
        vec4 clouds = marchClouds(cameraPos, rd, sunDir, cloudCoverage, cloudDensity);
        clouds.rgb *= cloudColor * currentTint * currentExposure;
        // Horizon fade so the slab edge isn't visible.
        float horizFade = smoothstep(0.03, 0.25, rd.y);
        clouds.a *= dayFactor * horizFade;
        finalColor = mix(finalColor, clouds.rgb, clamp(clouds.a, 0.0, 1.0));
    }

    FragColor = vec4(max(finalColor, vec3(0.0)) * weatherExposure, 1.0 - underwaterFactor);
}
