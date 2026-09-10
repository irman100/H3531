/* H3531 SDL PONG
 * Original native Linux/SDL 1.2 application for H3531 Home Computer.
 * SPDX-License-Identifier: MIT
 */
#include <SDL.h>
#include <unistd.h>
#include <stdlib.h>

/* 640x360 is exactly 16:9. The H3531 backend scales this 2x to the
 * board's fixed 1280x720 framebuffer, avoiding letterbox bars and fractional
 * dirty-rectangle scaling during gameplay. */
#define W 640
#define H 360

typedef struct { int x,y,w,h; } Box;

static SDL_Surface *screen;
static SDL_Surface *background;
static Uint32 black_c, white_c, cyan_c, red_c, green_c, yellow_c;

static SDL_Rect box_rect(Box b) {
    SDL_Rect r;
    r.x=(Sint16)b.x; r.y=(Sint16)b.y; r.w=(Uint16)b.w; r.h=(Uint16)b.h;
    return r;
}

static void fill_on(SDL_Surface *s, Box b, Uint32 c) {
    SDL_Rect r=box_rect(b);
    SDL_FillRect(s,&r,c);
}

static void fill(Box b, Uint32 c) {
    fill_on(screen,b,c);
}

static void seg_on(SDL_Surface *s,int x,int y,int n,Uint32 c) {
    static const unsigned char m[10]={0x3f,0x06,0x5b,0x4f,0x66,0x6d,0x7d,0x07,0x7f,0x6f};
    unsigned char a=m[n%10];
    Box q[7]={{x+3,y,18,4},{x+21,y+3,4,18},{x+21,y+24,4,18},{x+3,y+42,18,4},{x,y+24,4,18},{x,y+3,4,18},{x+3,y+21,18,4}};
    int i;
    for(i=0;i<7;i++) if(a&(1u<<i)) fill_on(s,q[i],c);
}

static void score_draw_on(SDL_Surface *s,int a,int b) {
    Box area={W/2-90,8,180,52};
    fill_on(s,area,black_c);
    seg_on(s,W/2-72,10,(a/10)%10,cyan_c);
    seg_on(s,W/2-42,10,a%10,cyan_c);
    seg_on(s,W/2+18,10,(b/10)%10,red_c);
    seg_on(s,W/2+48,10,b%10,red_c);
}

static void build_background(int ps,int as) {
    int y;
    SDL_FillRect(background,NULL,black_c);
    fill_on(background,(Box){8,8,W-16,4},white_c);
    fill_on(background,(Box){8,H-12,W-16,4},white_c);
    fill_on(background,(Box){8,8,4,H-16},white_c);
    fill_on(background,(Box){W-12,8,4,H-16},white_c);
    for(y=72;y<H-40;y+=28) fill_on(background,(Box){W/2-2,y,4,14},green_c);
    score_draw_on(background,ps,as);
}

static void restore(Box b) {
    SDL_Rect r=box_rect(b);
    SDL_BlitSurface(background,&r,screen,&r);
}

static void full_scene(Box player,Box ai,Box ball,int ps,int as) {
    SDL_BlitSurface(background,NULL,screen,NULL);
    fill(player,cyan_c); fill(ai,red_c); fill(ball,yellow_c);
    SDL_UpdateRect(screen,0,0,0,0);
}

static void push_rect(SDL_Rect *r,int *n,Box b) {
    if(b.x<0){b.w+=b.x;b.x=0;} if(b.y<0){b.h+=b.y;b.y=0;}
    if(b.x+b.w>W)b.w=W-b.x; if(b.y+b.h>H)b.h=H-b.y;
    if(b.w<=0||b.h<=0||*n>=20)return;
    r[*n].x=(Sint16)b.x; r[*n].y=(Sint16)b.y; r[*n].w=(Uint16)b.w; r[*n].h=(Uint16)b.h; (*n)++;
}

int main(int argc,char **argv) {
    Box player={W/2-58,H-38,116,12}, ai={W/2-50,26,100,10}, ball={W/2-7,H/2-7,14,14};
    int vx=5,vy=4, left=0,right=0,quit=0, ps=0,as=0, mouse_x=-1;
    (void)argc;(void)argv;
    if(SDL_Init(SDL_INIT_VIDEO)<0) return 2;
    screen=SDL_SetVideoMode(W,H,32,SDL_SWSURFACE);
    if(!screen){SDL_Quit();return 3;}
    SDL_ShowCursor(SDL_DISABLE);
    black_c=SDL_MapRGB(screen->format,0,0,0);
    white_c=SDL_MapRGB(screen->format,255,255,255);
    cyan_c=SDL_MapRGB(screen->format,0,220,255);
    red_c=SDL_MapRGB(screen->format,255,70,70);
    green_c=SDL_MapRGB(screen->format,80,210,100);
    yellow_c=SDL_MapRGB(screen->format,255,220,40);

    background=SDL_CreateRGBSurface(SDL_SWSURFACE,W,H,32,
        screen->format->Rmask,screen->format->Gmask,screen->format->Bmask,screen->format->Amask);
    if(!background){SDL_Quit();return 4;}
    build_background(ps,as);
    full_scene(player,ai,ball,ps,as);

    while(!quit) {
        SDL_Event e;
        Box op=player, oa=ai, ob=ball;
        SDL_Rect dirty[20]; int nd=0;
        int score_changed=0;
        while(SDL_PollEvent(&e)) {
            if(e.type==SDL_QUIT) quit=1;
            else if(e.type==SDL_KEYDOWN) {
                if(e.key.keysym.sym==SDLK_ESCAPE || e.key.keysym.sym==SDLK_q) quit=1;
                if(e.key.keysym.sym==SDLK_LEFT || e.key.keysym.sym==SDLK_a) left=1;
                if(e.key.keysym.sym==SDLK_RIGHT || e.key.keysym.sym==SDLK_d) right=1;
            } else if(e.type==SDL_KEYUP) {
                if(e.key.keysym.sym==SDLK_LEFT || e.key.keysym.sym==SDLK_a) left=0;
                if(e.key.keysym.sym==SDLK_RIGHT || e.key.keysym.sym==SDLK_d) right=0;
            } else if(e.type==SDL_MOUSEMOTION) mouse_x=e.motion.x;
        }
        if(mouse_x>=0) player.x=mouse_x-player.w/2;
        else { if(left)player.x-=9; if(right)player.x+=9; }
        if(player.x<16)player.x=16; if(player.x+player.w>W-16)player.x=W-16-player.w;

        if(ball.x+ball.w/2 < ai.x+ai.w/2-5) ai.x-=4;
        if(ball.x+ball.w/2 > ai.x+ai.w/2+5) ai.x+=4;
        if(ai.x<16)ai.x=16; if(ai.x+ai.w>W-16)ai.x=W-16-ai.w;

        ball.x+=vx; ball.y+=vy;
        if(ball.x<=14){ball.x=14;vx=abs(vx);} if(ball.x+ball.w>=W-14){ball.x=W-14-ball.w;vx=-abs(vx);}
        if(vy>0 && ball.y+ball.h>=player.y && ball.y<player.y+player.h && ball.x+ball.w>=player.x && ball.x<=player.x+player.w){
            ball.y=player.y-ball.h; vy=-abs(vy); vx+=(ball.x+ball.w/2-(player.x+player.w/2))/24; if(vx>9)vx=9;if(vx<-9)vx=-9;
        }
        if(vy<0 && ball.y<=ai.y+ai.h && ball.y+ball.h>ai.y && ball.x+ball.w>=ai.x && ball.x<=ai.x+ai.w){
            ball.y=ai.y+ai.h; vy=abs(vy);
        }
        if(ball.y<8){ ps++; ball.x=W/2-7;ball.y=H/2-7;vx=(ps&1)?5:-5;vy=4; score_changed=1; }
        if(ball.y>H){ as++; ball.x=W/2-7;ball.y=H/2-7;vx=(as&1)?-5:5;vy=-4; score_changed=1; }

        /* Never erase moving objects with flat black: that destroys static UI
           beneath them (centre dashed line, borders and score digits). Restore
           their old rectangles from a clean background surface instead. */
        if(score_changed) {
            score_draw_on(background,ps,as);
            push_rect(dirty,&nd,(Box){W/2-90,8,180,52});
        }
        restore(op); restore(oa); restore(ob);
        fill(player,cyan_c); fill(ai,red_c); fill(ball,yellow_c);
        push_rect(dirty,&nd,op); push_rect(dirty,&nd,oa); push_rect(dirty,&nd,ob);
        push_rect(dirty,&nd,player); push_rect(dirty,&nd,ai); push_rect(dirty,&nd,ball);
        if(nd) SDL_UpdateRects(screen,nd,dirty);
        usleep(12000);
    }
    SDL_FreeSurface(background);
    SDL_Quit();
    return 0;
}
