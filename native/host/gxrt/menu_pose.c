/* Original Melee menu camera -> overlay-plane homography.
 * C-stick changes the eye in mn_8022BA1C; use those exact live coordinates.
 * Homography maps the UI's neutral800x600canvas onto worldz17.2, the native
 * header/frame plane, so text and textured foreground track camera rotation. */
#include "abi_recompcore.h"
#include <math.h>
#include <stdint.h>
#include <string.h>
static int pose_valid(uint32_t p,unsigned n){return p>=0x80000000u&&(uint64_t)p+n<=0x81800000u;}
static double pose_float(Context* c,uint32_t p){uint32_t v=mem_read32(c,p);float f;memcpy(&f,&v,4);return f;}
static void pose_vec(Context*c,uint32_t p,double v[3]){for(int i=0;i<3;i++)v[i]=pose_float(c,p+4*i);}
static double pose_dot(const double a[3],const double b[3]){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
static int pose_normal(double v[3]){double n=sqrt(pose_dot(v,v));if(!isfinite(n)||n<1e-6)return 0;for(int i=0;i<3;i++)v[i]/=n;return 1;}
static void pose_cross(const double a[3],const double b[3],double v[3]){v[0]=a[1]*b[2]-a[2]*b[1];v[1]=a[2]*b[0]-a[0]*b[2];v[2]=a[0]*b[1]-a[1]*b[0];}
static int pose_plane(const double eye[3],const double aim[3],double fov,double out[9]){
 double z[3],x[3],y[3],up[3]={0,1,0};for(int i=0;i<3;i++)z[i]=eye[i]-aim[i];
 if(!pose_normal(z))return 0;pose_cross(up,z,x);if(!pose_normal(x))return 0;pose_cross(z,x,y);
 double r[3][3]={{x[0],x[1],17.2*x[2]-pose_dot(x,eye)},{y[0],y[1],17.2*y[2]-pose_dot(y,eye)},{z[0],z[1],17.2*z[2]-pose_dot(z,eye)}};
 if(!(fov>1&&fov<170))return 0;double s=300.0/tan(fov*0.008726646259971648);
 for(int i=0;i<3;i++){out[i]=s*r[0][i]-400*r[2][i];out[3+i]=-s*r[1][i]-300*r[2][i];out[6+i]=-r[2][i];}return 1;
}
static int pose_inverse(const double a[9],double o[9]){
 o[0]=a[4]*a[8]-a[5]*a[7];o[1]=a[2]*a[7]-a[1]*a[8];o[2]=a[1]*a[5]-a[2]*a[4];
 o[3]=a[5]*a[6]-a[3]*a[8];o[4]=a[0]*a[8]-a[2]*a[6];o[5]=a[2]*a[3]-a[0]*a[5];
 o[6]=a[3]*a[7]-a[4]*a[6];o[7]=a[1]*a[6]-a[0]*a[7];o[8]=a[0]*a[4]-a[1]*a[3];
 double det=a[0]*o[0]+a[1]*o[3]+a[2]*o[6];if(!isfinite(det)||fabs(det)<1e-8)return 0;for(int i=0;i<9;i++)o[i]/=det;return 1;
}
int menu_pose_read(Context* ctx,float out[9]){
 for(int i=0;i<9;i++)out[i]=(i%4)==0?1.f:0.f;
 uint32_t desc=mem_read32(ctx,0x804D6BC4u),gobj=mem_read32(ctx,0x804D6BACu);
 if(!pose_valid(desc,0x38)||!pose_valid(gobj,0x30)||mem_read16(ctx,desc+6)!=1)return 0;
 uint32_t cobj=mem_read32(ctx,gobj+0x28);if(!pose_valid(cobj,0x90)||mem_read8(ctx,cobj+0x50)!=1)return 0;
 uint32_t base_eye=mem_read32(ctx,desc+0x18),base_aim=mem_read32(ctx,desc+0x1C);
 uint32_t live_eye=mem_read32(ctx,cobj+0x24),live_aim=mem_read32(ctx,cobj+0x28);
 if(!pose_valid(base_eye,0x14)||!pose_valid(base_aim,0x14)||!pose_valid(live_eye,0x18)||!pose_valid(live_aim,0x18))return 0;
 double eye[3],aim[3],base[9],live[9],inv[9],result[9];
 pose_vec(ctx,base_eye+4,eye);pose_vec(ctx,base_aim+4,aim);if(!pose_plane(eye,aim,pose_float(ctx,desc+0x30),base))return 0;
 pose_vec(ctx,live_eye+0xC,eye);pose_vec(ctx,live_aim+0xC,aim);if(!pose_plane(eye,aim,pose_float(ctx,cobj+0x40),live)||!pose_inverse(base,inv))return 0;
 for(int row=0;row<3;row++)for(int col=0;col<3;col++){double v=0;for(int k=0;k<3;k++)v+=live[row*3+k]*inv[k*3+col];result[row*3+col]=v;}
 if(!isfinite(result[8])||fabs(result[8])<1e-6)return 0;
 for(int i=0;i<9;i++){double v=result[i]/result[8];if(!isfinite(v))return 0;out[i]=(float)v;}return 1;
}