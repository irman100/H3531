/* H3531 SDL 1.2 evdev backend patch.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "SDL_config.h"
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <linux/input.h>
#include "SDL.h"
#include "../../events/SDL_sysevents.h"
#include "../../events/SDL_events_c.h"
#include "SDL_nullvideo.h"
#include "SDL_nullevents_c.h"

static int kfd=-1,mfd=-1;
static int shift_l=0,shift_r=0,ctrl_l=0,ctrl_r=0,alt_l=0,alt_r=0,caps=0;
static long long next_scan_ms=0;
static int mdx=0,mdy=0;
static Uint8 mouse_buttons=0;

static long long nowms(void){struct timeval tv;gettimeofday(&tv,0);return (long long)tv.tv_sec*1000LL+tv.tv_usec/1000;}
static int contains_ci(const char *s,const char *q){int i,j;for(i=0;s[i];i++){for(j=0;q[j]&&s[i+j];j++){char a=s[i+j],b=q[j];if(a>='A'&&a<='Z')a+=32;if(b>='A'&&b<='Z')b+=32;if(a!=b)break;}if(!q[j])return 1;}return 0;}
static int open_kind(int mouse){int i;char p[64],name[128];for(i=0;i<64;i++){int fd;SDL_snprintf(p,sizeof(p),"/dev/input/event%d",i);fd=open(p,O_RDONLY|O_NONBLOCK,0);if(fd<0)continue;SDL_memset(name,0,sizeof(name));if(ioctl(fd,EVIOCGNAME(sizeof(name)-1),name)<0){close(fd);continue;}if(mouse?contains_ci(name,"mouse"):(contains_ci(name,"keyboard")||contains_ci(name," kb")))return fd;close(fd);}return -1;}
static void rescan(void){long long n=nowms();if(n<next_scan_ms)return;next_scan_ms=n+1500;if(kfd<0)kfd=open_kind(0);if(mfd<0)mfd=open_kind(1);}
static SDLMod mods(void){SDLMod m=KMOD_NONE;if(shift_l)m|=KMOD_LSHIFT;if(shift_r)m|=KMOD_RSHIFT;if(ctrl_l)m|=KMOD_LCTRL;if(ctrl_r)m|=KMOD_RCTRL;if(alt_l)m|=KMOD_LALT;if(alt_r)m|=KMOD_RALT;if(caps)m|=KMOD_CAPS;return m;}

static SDLKey sym_for(unsigned c){switch(c){
case KEY_ESC:return SDLK_ESCAPE;case KEY_1:return SDLK_1;case KEY_2:return SDLK_2;case KEY_3:return SDLK_3;case KEY_4:return SDLK_4;case KEY_5:return SDLK_5;case KEY_6:return SDLK_6;case KEY_7:return SDLK_7;case KEY_8:return SDLK_8;case KEY_9:return SDLK_9;case KEY_0:return SDLK_0;
case KEY_MINUS:return SDLK_MINUS;case KEY_EQUAL:return SDLK_EQUALS;case KEY_BACKSPACE:return SDLK_BACKSPACE;case KEY_TAB:return SDLK_TAB;case KEY_Q:return SDLK_q;case KEY_W:return SDLK_w;case KEY_E:return SDLK_e;case KEY_R:return SDLK_r;case KEY_T:return SDLK_t;case KEY_Y:return SDLK_y;case KEY_U:return SDLK_u;case KEY_I:return SDLK_i;case KEY_O:return SDLK_o;case KEY_P:return SDLK_p;
case KEY_LEFTBRACE:return SDLK_LEFTBRACKET;case KEY_RIGHTBRACE:return SDLK_RIGHTBRACKET;case KEY_ENTER:return SDLK_RETURN;case KEY_LEFTCTRL:return SDLK_LCTRL;case KEY_A:return SDLK_a;case KEY_S:return SDLK_s;case KEY_D:return SDLK_d;case KEY_F:return SDLK_f;case KEY_G:return SDLK_g;case KEY_H:return SDLK_h;case KEY_J:return SDLK_j;case KEY_K:return SDLK_k;case KEY_L:return SDLK_l;case KEY_SEMICOLON:return SDLK_SEMICOLON;case KEY_APOSTROPHE:return SDLK_QUOTE;case KEY_GRAVE:return SDLK_BACKQUOTE;
case KEY_LEFTSHIFT:return SDLK_LSHIFT;case KEY_BACKSLASH:return SDLK_BACKSLASH;case KEY_Z:return SDLK_z;case KEY_X:return SDLK_x;case KEY_C:return SDLK_c;case KEY_V:return SDLK_v;case KEY_B:return SDLK_b;case KEY_N:return SDLK_n;case KEY_M:return SDLK_m;case KEY_COMMA:return SDLK_COMMA;case KEY_DOT:return SDLK_PERIOD;case KEY_SLASH:return SDLK_SLASH;case KEY_RIGHTSHIFT:return SDLK_RSHIFT;case KEY_LEFTALT:return SDLK_LALT;case KEY_SPACE:return SDLK_SPACE;case KEY_CAPSLOCK:return SDLK_CAPSLOCK;
case KEY_F1:return SDLK_F1;case KEY_F2:return SDLK_F2;case KEY_F3:return SDLK_F3;case KEY_F4:return SDLK_F4;case KEY_F5:return SDLK_F5;case KEY_F6:return SDLK_F6;case KEY_F7:return SDLK_F7;case KEY_F8:return SDLK_F8;case KEY_F9:return SDLK_F9;case KEY_F10:return SDLK_F10;case KEY_F11:return SDLK_F11;case KEY_F12:return SDLK_F12;
case KEY_RIGHTCTRL:return SDLK_RCTRL;case KEY_RIGHTALT:return SDLK_RALT;case KEY_HOME:return SDLK_HOME;case KEY_UP:return SDLK_UP;case KEY_PAGEUP:return SDLK_PAGEUP;case KEY_LEFT:return SDLK_LEFT;case KEY_RIGHT:return SDLK_RIGHT;case KEY_END:return SDLK_END;case KEY_DOWN:return SDLK_DOWN;case KEY_PAGEDOWN:return SDLK_PAGEDOWN;case KEY_INSERT:return SDLK_INSERT;case KEY_DELETE:return SDLK_DELETE;default:return SDLK_UNKNOWN;}}

static Uint16 uni_for(unsigned c){int sh=shift_l||shift_r;int upper=sh^caps;switch(c){
case KEY_A:return upper?'A':'a';case KEY_B:return upper?'B':'b';case KEY_C:return upper?'C':'c';case KEY_D:return upper?'D':'d';case KEY_E:return upper?'E':'e';case KEY_F:return upper?'F':'f';case KEY_G:return upper?'G':'g';case KEY_H:return upper?'H':'h';case KEY_I:return upper?'I':'i';case KEY_J:return upper?'J':'j';case KEY_K:return upper?'K':'k';case KEY_L:return upper?'L':'l';case KEY_M:return upper?'M':'m';case KEY_N:return upper?'N':'n';case KEY_O:return upper?'O':'o';case KEY_P:return upper?'P':'p';case KEY_Q:return upper?'Q':'q';case KEY_R:return upper?'R':'r';case KEY_S:return upper?'S':'s';case KEY_T:return upper?'T':'t';case KEY_U:return upper?'U':'u';case KEY_V:return upper?'V':'v';case KEY_W:return upper?'W':'w';case KEY_X:return upper?'X':'x';case KEY_Y:return upper?'Y':'y';case KEY_Z:return upper?'Z':'z';
case KEY_1:return sh?'!':'1';case KEY_2:return sh?'@':'2';case KEY_3:return sh?'#':'3';case KEY_4:return sh?'$':'4';case KEY_5:return sh?'%':'5';case KEY_6:return sh?'^':'6';case KEY_7:return sh?'&':'7';case KEY_8:return sh?'*':'8';case KEY_9:return sh?'(':'9';case KEY_0:return sh?')':'0';case KEY_MINUS:return sh?'_':'-';case KEY_EQUAL:return sh?'+':'=';case KEY_LEFTBRACE:return sh?'{':'[';case KEY_RIGHTBRACE:return sh?'}':']';case KEY_BACKSLASH:return sh?'|':'\\';case KEY_SEMICOLON:return sh?':':';';case KEY_APOSTROPHE:return sh?'"':'\'';case KEY_GRAVE:return sh?'~':'`';case KEY_COMMA:return sh?'<':',';case KEY_DOT:return sh?'>':'.';case KEY_SLASH:return sh?'?':'/';case KEY_SPACE:return ' ';case KEY_TAB:return '\t';case KEY_ENTER:return '\r';case KEY_BACKSPACE:return '\b';default:return 0;}}

static void key_event(struct input_event *e){SDL_keysym k;int press=e->value?1:0;if(e->value==2)return;switch(e->code){case KEY_LEFTSHIFT:shift_l=press;break;case KEY_RIGHTSHIFT:shift_r=press;break;case KEY_LEFTCTRL:ctrl_l=press;break;case KEY_RIGHTCTRL:ctrl_r=press;break;case KEY_LEFTALT:alt_l=press;break;case KEY_RIGHTALT:alt_r=press;break;case KEY_CAPSLOCK:if(press)caps=!caps;break;default:break;}SDL_memset(&k,0,sizeof(k));k.scancode=(Uint8)(e->code&255);k.sym=sym_for(e->code);k.mod=mods();k.unicode=press?uni_for(e->code):0;if(k.sym!=SDLK_UNKNOWN)SDL_PrivateKeyboard(press?SDL_PRESSED:SDL_RELEASED,&k);}
static void mouse_key(struct input_event *e){Uint8 b=0;if(e->code==BTN_LEFT)b=SDL_BUTTON_LEFT;else if(e->code==BTN_RIGHT)b=SDL_BUTTON_RIGHT;else if(e->code==BTN_MIDDLE)b=SDL_BUTTON_MIDDLE;if(b){if(e->value)mouse_buttons|=SDL_BUTTON(b);else mouse_buttons&=~SDL_BUTTON(b);SDL_PrivateMouseButton(e->value?SDL_PRESSED:SDL_RELEASED,b,0,0);}}
static void pumpfd(int fd,int mouse){struct input_event e;for(;;){ssize_t n=read(fd,&e,sizeof(e));if(n==(ssize_t)sizeof(e)){if(mouse){if(e.type==EV_REL){if(e.code==REL_X)mdx+=e.value;else if(e.code==REL_Y)mdy+=e.value;else if(e.code==REL_WHEEL&&e.value){Uint8 b=e.value>0?SDL_BUTTON_WHEELUP:SDL_BUTTON_WHEELDOWN;SDL_PrivateMouseButton(SDL_PRESSED,b,0,0);SDL_PrivateMouseButton(SDL_RELEASED,b,0,0);}}else if(e.type==EV_KEY)mouse_key(&e);else if(e.type==EV_SYN&&e.code==SYN_REPORT&&(mdx||mdy)){SDL_PrivateMouseMotion(mouse_buttons,1,(Sint16)mdx,(Sint16)mdy);mdx=mdy=0;}}else if(e.type==EV_KEY)key_event(&e);continue;}if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))break;if(mouse){close(mfd);mfd=-1;}else{close(kfd);kfd=-1;}break;}}
void DUMMY_PumpEvents(_THIS){(void)this;rescan();if(kfd>=0)pumpfd(kfd,0);if(mfd>=0)pumpfd(mfd,1);}
void DUMMY_InitOSKeymap(_THIS){(void)this;rescan();}
