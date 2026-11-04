// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
layout(set=0,binding=0) uniform sampler2D image;
layout(push_constant) uniform Grade {float exposure;float contrast;float saturation;int preset;float grain;float vignette;int frame;float strength;int encoded;uint width;uint height;} grade;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
vec3 decodeRGB(vec3 c){return mix(c/12.92,pow((c+.055)/1.055,vec3(2.4)),greaterThan(c,vec3(.04045)));}
vec3 encodeRGB(vec3 c){c=clamp(c,0,1);return mix(c*12.92,1.055*pow(c,vec3(1.0/2.4))-.055,greaterThan(c,vec3(.0031308)));}
uint filmHash(uint x,uint y){uint h=x*0x9e3779b9u^y*0x85ebca6bu^0x1954u;h^=h>>16;h*=0x7feb352du;h^=h>>15;h*=0x846ca68bu;return h^(h>>16);}
void main(){
    vec4 source=texture(image,uv);
    if(grade.frame!=0){
        const float ratios[6]=float[6](0,2.39,21.0/9.0,1,.8,4.0/3.0);
        float ratio=ratios[clamp(grade.frame,0,5)],aspect=float(grade.width)/grade.height;
        vec2 visible=aspect>ratio?vec2(ratio/aspect,1):vec2(1,aspect/ratio);
        // Match CPU capture pixel centers even when the preview is resized.
        vec2 center=(floor(uv*vec2(grade.width,grade.height))+.5)/vec2(grade.width,grade.height);
        if(any(lessThan(center,(1-visible)*.5)) || any(greaterThan(center,(1+visible)*.5))){color=vec4(0,0,0,source.a);return;}
    }
    vec3 input_color=source.rgb;
    if(grade.encoded!=0)input_color=decodeRGB(input_color);
    vec3 c=grade.contrast==0?vec3(.18):.18*pow(max(input_color*exp2(grade.exposure),vec3(0))/.18,vec3(grade.contrast));
    float l=dot(c,vec3(.2126,.7152,.0722));
    c=mix(vec3(l),c,grade.saturation);
    vec3 base_color=c;
    if(grade.preset==1)c=vec3(l);
    else if(grade.preset==2)c=vec3(dot(c,vec3(.393,.769,.189)),dot(c,vec3(.349,.686,.168)),dot(c,vec3(.272,.534,.131)));
    else if(grade.preset==3)c*=vec3(.9,1,1.1);
    else if(grade.preset==4)c*=vec3(1.1,1,.9);
    else if(grade.preset==5)c=vec3(.006+.18*pow(max(l,0)/.18,1.18));
    else if(grade.preset==6)c=vec3(.015,.010,.015)+mix(vec3(l),c,1.12)*vec3(1.08,1.02,.83);
    else if(grade.preset==7){float shadow=1-clamp(l*2,0,1);c*=vec3(1.03-.25*shadow,.95+.15*shadow,1.15+.15*shadow);}
    else if(grade.preset==8){float shadow=1-clamp(l*2,0,1);c=mix(vec3(l),c,1.16)*vec3(1.06-.10*shadow,1.02+.02*shadow,.97+.13*shadow);}
    c=clamp(mix(base_color,c,grade.strength),0,1);
    if(grade.grain!=0 || grade.vignette!=0) {
        c=encodeRGB(c);
        uvec2 p=uvec2(clamp(floor(uv*vec2(grade.width,grade.height)),vec2(0),vec2(grade.width-1,grade.height-1)));
        float noise=(float(filmHash(p.x,p.y)&65535u)/65535-.5)*grade.grain*.16;
        if(grade.preset==5 && grade.grain>0) {
            if(filmHash(p.x/2,37)%503u==0 && (filmHash(p.x/2,p.y/12)&3u)!=0)noise+=grade.grain*grade.strength*.12;
            if(filmHash(p.x/3,p.y/3)%4093u==0)noise-=grade.grain*grade.strength*.25;
        }
        vec2 position=2*(vec2(p)+.5)/vec2(grade.width,grade.height)-1;
        float radius=min(1,dot(position,position)*.5);
        c=clamp(c*(1-grade.vignette*.65*radius*radius)+noise,0,1);
        if(grade.encoded==0)c=decodeRGB(c);
    } else if(grade.encoded!=0)c=encodeRGB(c);
    color=vec4(c,source.a);
}
