/*!
 * @file        mainsdl.cpp
 * @author      mithrendal and Dirk W. Hoffmann, www.dirkwhoffmann.de
 * @copyright   Dirk W. Hoffmann. All rights reserved.
 */
 
#include <emscripten/wasm_worker.h>
#include <stdio.h>
#include "config.h"
#include "Amiga.h"
#include "AmigaTypes.h"
#include "RomFile.h"
#include "ExtendedRomFile.h"
#include "ADFFile.h"
#include "DMSFile.h"
#include "EXEFile.h"
#include "HDFFile.h"
#include "Snapshot.h"

#include "MemUtils.h"


#include <emscripten.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengles2.h> 
#include <emscripten/html5.h>

#define RENDER_SOFTWARE 0
#define RENDER_GPU 1
#define RENDER_SHADER 2
u8 render_method = RENDER_SOFTWARE;

#define DISPLAY_NARROW   0
#define DISPLAY_STANDARD 1
#define DISPLAY_WIDER    2
#define DISPLAY_OVERSCAN 3
#define DISPLAY_ADAPTIVE 4
#define DISPLAY_BORDERLESS 5
u8 geometry  = DISPLAY_ADAPTIVE;


const char* display_names[] = {"narrow","standard","wider","overscan",
"viewport tracking","borderless"}; 

bool log_on=false;


//HRM: In NTSC, the fields are 262, then 263 lines and in PAL, 312, then 313 lines.
//312-50=262
#define PAL_EXTRA_VPIXEL 50

bool ntsc=false;
u8 target_fps = 50;

extern "C" void wasm_set_display(const char *name);
extern "C" void wasm_execute_in_worker();

int clip_width  = 724 * TPP;
int clip_height = 568;
int clip_offset = 0;//HBLANK_MIN*4;//HBLANK_MAX*4; 
int buffer_size = 4096;

bool prevLOF = false;
bool currLOF = false;




int eat_border_width = 0;
int eat_border_height = 0;
int xOff = 12 + eat_border_width;
int yOff = 12 + eat_border_height;
int clipped_width  = HPIXELS*TPP -12 -24 -2*eat_border_width; //392
int clipped_height = VPOS_MAX -12 -24 -2*eat_border_height; //248


char *filename = NULL;


unsigned int warp_to_frame=0;
int sum_samples=0;
double last_time = 0.0 ;
double last_time_calibrated = 0.0 ;
unsigned int executed_frame_count=0;
int64_t total_executed_frame_count=0;
double start_time=0;//emscripten_get_now();
unsigned int rendered_frame_count=0;
unsigned int frames=0, seconds=0;
// The emscripten "main loop" replacement function.

u16 vstart_min=26;
u16 vstop_max=256;
u16 hstart_min=200;
u16 hstop_max=HPIXELS;

u16 vstart_min_tracking=26;
u16 vstop_max_tracking=256;
u16 hstart_min_tracking=200;
u16 hstop_max_tracking=HPIXELS;
bool reset_calibration=true;
bool request_to_reset_calibration=false;

void set_viewport_dimensions()
{
}



u16 vstart_min_calib=0;
u16 vstop_max_calib=0;
u16 hstart_min_calib=0;
u16 hstop_max_calib=0;
bool calculate_viewport_dimensions(Uint32 *texture)
{
  if(reset_calibration)
  {
      //printf("reset_calibration %d %d\n",vstop_max_tracking, vstart_min_tracking);
      //first call after new viewport size
      //set start values to the opposite max. borderpos (scan area)  
      vstart_min_calib = vstop_max_tracking; 
      vstop_max_calib = vstart_min_tracking;
      hstart_min_calib = hstop_max_tracking;
      hstop_max_calib = hstart_min_tracking;  
      reset_calibration=false;

  }

  bool pixels_found=false;
  //top border: get vstart_min from texture
  Uint32 ref_pixel= texture[(HPIXELS*vstart_min_tracking + hstart_min_tracking)*TPP];
//    printf("refpixel:%u\n",ref_pixel);
  for(int y=vstart_min_tracking;y<vstart_min_calib && !pixels_found;y++)
  {
//      printf("\nvstart_line:%u\n",y);
    for(int x=hstart_min_tracking;x<hstop_max_tracking;x++){
      Uint32 pixel= texture[(HPIXELS*y + x)*TPP];
//        printf("%u:%u ",x,pixel);
      if(ref_pixel != pixel){
        pixels_found=true;
//        printf("\nfirst_pos=%d, vstart_min=%d, vstart_min_track=%d\n",y, vstart_min, vstart_min_tracking);
        vstart_min_calib= y<vstart_min_calib?y:vstart_min_calib;
        break;
      }
    }
  }
  
  //bottom border: get vstop_max from texture
  pixels_found=false;
  ref_pixel= texture[ (HPIXELS*vstop_max_tracking + hstart_min_tracking)*TPP];
//    printf("refpixel:%u\n",ref_pixel);
//    printf("hstart:%u,hstop:%u\n",hstart_min,hstop_max);
  
  for(int y=vstop_max_tracking;y>vstop_max_calib && !pixels_found;y--)
  {
//      printf("\nline:%u\n",y);
    for(int x=hstart_min_tracking;x<hstop_max_tracking;x++){
      Uint32 pixel= texture[(HPIXELS*y + x)*TPP];
//        printf("%u:%u ",x,pixel);
      if(ref_pixel != pixel){
        pixels_found=true;
        y++; //this line has pixels, so put vstop_max to the next line
//        printf("\ncalib: last_pos=%d, vstop_max=%d, vstop_max_tracking%d\n",y, vstop_max, vstop_max_tracking);
        vstop_max_calib= y>vstop_max_calib?y:vstop_max_calib;
        break;
      }
    }
  }

  //left border: get hstart_min from texture
  pixels_found=false;
  ref_pixel= texture[ (HPIXELS*vstart_min_tracking + hstart_min_tracking)*TPP];

  for(int x=hstart_min_tracking;x<hstart_min_calib;x++)
  {
//      printf("\nrow:%u\n",x);
    for(int y=vstart_min_calib;y<vstop_max_calib && !pixels_found;y++)
    {
      Uint32 pixel= texture[(HPIXELS*y + x)*TPP];
//        printf("%u:%u ",x,pixel);
      if(ref_pixel != pixel){
        pixels_found=true;
//          printf("\nlast_xpos=%d, hstop_max=%d\n",x, hstop_max);
        hstart_min_calib=x<hstart_min_calib?x:hstart_min_calib;
        break;
      }
    }
  }

  //right border: get hstop_max from texture
  pixels_found=false;
  ref_pixel= texture[ (HPIXELS*vstart_min_tracking + hstop_max_tracking)*TPP];
//  printf("hstop_max_tracking %d\n",hstop_max_tracking);
//  printf("hstop_max_calib %d\n",hstop_max_calib);
//  printf("ref_pixel %d\n",ref_pixel);

  for(int x=hstop_max_tracking;x>hstop_max_calib;x--)
  {
//     printf("\ncol:%u\n",x);
    for(int y=vstart_min_calib;y<vstop_max_calib && !pixels_found;y++)
    {
      Uint32 pixel= texture[(HPIXELS*y + x)*TPP];
//      printf("%u:%u ",x,pixel);
      if(ref_pixel != pixel){
        pixels_found=true;
        x++; //this line has pixels, so put hstop_max to the next line
//          printf("\nlast_xpos=%d, hstop_max=%d\n",x, hstop_max);
        hstop_max_calib= x>hstop_max_calib?x:hstop_max_calib;
        break;
      }
    }
  }
//  printf("->hstop_max_calib %d\n",hstop_max_calib);

  bool dimensions_changed=false;

  //handdisk start pixel 416, stop pixel 676
  #define HANDSTART_PIXEL 416
  #define HANDSTOP_PIXEL 676


  if(hstart_min_calib > HANDSTART_PIXEL-32)
    hstart_min_calib= HANDSTART_PIXEL-32;
  if(hstop_max_calib < HANDSTOP_PIXEL+32)
    hstop_max_calib= HANDSTOP_PIXEL+32;

  if(hstart_min_calib < hstop_max_calib )
  {
    if(hstart_min!=hstart_min_calib)
    {
      hstart_min=hstart_min_calib;
      dimensions_changed=true;
    }
    if(hstop_max != hstop_max_calib)
    {
      hstop_max = hstop_max_calib;
      dimensions_changed=true;
    }
  }

  if(vstart_min_calib > VPOS_MAX/4)
    vstart_min_calib= VPOS_MAX/4;
  if(vstop_max_calib < 3*VPOS_MAX/4)
    vstop_max_calib= 3*VPOS_MAX/4;

  if(vstart_min_calib< vstop_max_calib)
  {
    if(vstart_min != vstart_min_calib)
    {
      vstart_min=vstart_min_calib;
      dimensions_changed=true;
    }
    if(vstop_max != vstop_max_calib)
    {
      vstop_max=vstop_max_calib;   
      dimensions_changed=true;
    }
 
  }
//  printf("\nCALIBRATED: (%d,%d) (%d,%d) \n",hstart_min, vstart_min, hstop_max, vstop_max);

  //printf("calib dimensions changed=%d\n",dimensions_changed);
  return dimensions_changed;
}


#define MAX_GAP 5
Amiga *thisAmiga=NULL;
u8 executed_since_last_host_frame=0;
extern "C" void wasm_execute()
{
  wasm_execute_in_worker();
}

extern "C" int wasm_draw_one_frame(double now)
//int draw_one_frame_into_SDL(void *thisAmiga, float now) 
{

  //this method is triggered by
  //emscripten_set_main_loop_arg(em_arg_callback_func func, void *arg, int fps, int simulate_infinite_loop) 
  //which is called inside te c64.cpp
  //fps Setting int <=0 (recommended) uses the browser’s requestAnimationFrame mechanism to call the function.

  //The number of callbacks is usually 60 times per second, but will 
  //generally match the display refresh rate in most web browsers as 
  //per W3C recommendation. requestAnimationFrame() 
  
  //double now = emscripten_get_now();  

  double elapsedTimeInSeconds = (now - start_time)/1000.0;
  int64_t targetFrameCount = (int64_t)(elapsedTimeInSeconds * target_fps);
 
  Amiga *amiga = /*(Amiga *)*/thisAmiga;
  bool show_stat=true;
  if(amiga->inWarpMode() == true)
  {
    if(log_on) printf("warping at least 25 frames at once ...\n");
    int i=25;
    while(amiga->inWarpMode() == true && i>0)
    {
      amiga->execute();
      i--;
      if(warp_to_frame>0 && amiga->agnus.pos.frame > warp_to_frame)
      {
        if(log_on) printf("reached warp_to_frame count\n");
        amiga->warpOff();
        warp_to_frame=0;
      }
    }
    start_time=now;
    total_executed_frame_count=0;
    targetFrameCount=1;
    show_stat=false;
  }
  if(show_stat)
  {
    //lost the sync
    if(targetFrameCount-total_executed_frame_count > MAX_GAP)
    {
        if(log_on) printf("lost sync target=%lld, total_executed=%lld\n", targetFrameCount, total_executed_frame_count);
        //reset timer
        //because we are out of sync, we do now skip max_gap-1 emulation frames 
        start_time=now;
        total_executed_frame_count=0;
        targetFrameCount=1;  //we are hoplessly behind but do at least one in this round  
    }

    if(now-last_time>= 1000.0)
    { 
      double passed_time= now - last_time;
      last_time = now;

      seconds += 1; 
      frames += rendered_frame_count;
      if(log_on) printf("time[ms]=%.0lf, audio_samples=%d, frames [executed=%u, rendered=%u] avg_fps=%u\n", 
      passed_time, sum_samples, executed_frame_count, rendered_frame_count, frames/seconds);
      sum_samples=0; 
      rendered_frame_count=0;
      executed_frame_count=0;
    }
  }

  int behind=targetFrameCount-total_executed_frame_count;    
  if(behind<=0 && executed_since_last_host_frame==0)
    return behind;   //don't render if ahead of time and everything is already drawn
  
  if(
    executed_since_last_host_frame%2==0 //when 0, 2, 4, ... execute here directly 
  )
  {
    //execute always an odd number of amiga frames
    //this is needed for correct detection of longframes and shortframes when 
    //amiga uses interlaced resolution and the host device is too slow to render 
    //always one amiga frame per host frame i.e. when fps drops below 50 
    
    //while(behind>0)
    //{
    //  wasm_execute_in_worker();
    //  behind--;
    //}
//    amiga->execute();
 
    //executed_frame_count++;
    //total_executed_frame_count++;
    //behind--;
  }

  executed_since_last_host_frame=0;
  rendered_frame_count++;   

  /*EM_ASM({
      draw_one_frame(); // to gather joystick information for example 
  });
  */
//  auto &stableBuffer = amiga->denise.pixelEngine.getStableBuffer();
//  auto stable_ptr = amiga->denise.pixelEngine.stablePtr();

/*
  if(geometry == DISPLAY_BORDERLESS)
  {
//    printf("calibration count=%f\n",now-last_time_calibrated);
    if(now-last_time_calibrated >= 700.0)
    {  
      last_time_calibrated=now;
      bool dimensions_changed=calculate_viewport_dimensions((Uint32 *)stable_ptr - HBLANK_MIN*4*TPP);
      if(dimensions_changed)
      {
        set_viewport_dimensions(); 
      }  

      if(request_to_reset_calibration)
      {
        reset_calibration=true;
        request_to_reset_calibration=false;
      }
    }
  }

  if(render_method==RENDER_SHADER)
  {
    prevLOF = currLOF;
    currLOF = stableBuffer.longFrame;

    if (currLOF) {
      glActiveTexture(GL_TEXTURE0);
    } else if (prevLOF) {
      glActiveTexture(GL_TEXTURE1);
    } else {
      glActiveTexture(GL_TEXTURE0);
    }  

    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, HPIXELS*TPP, VPOS_MAX, GL_RGBA, GL_UNSIGNED_BYTE, stable_ptr+clip_offset);

    if (currLOF != prevLOF) {
      // Case 1: Interlace drawing
      glUseProgram(merge);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, longf);

      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, shortf);
    } 
    else 
    {
      // Case 2: Non-interlace drawing (two long frames in a row)
      glUseProgram(basic);
      glBindTexture(GL_TEXTURE_2D, longf);
    } 

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    SDL_GL_SwapWindow(window);
  }
  else
  {

    Uint8 *texture = (Uint8 *)(stable_ptr)+clip_offset*4;

  //  SDL_RenderClear(renderer);
    SDL_Rect SrcR;

    SrcR.x = (xOff-HBLANK_MIN*4) *TPP;
    SrcR.y = yOff;
    SrcR.w = clipped_width * TPP;
    SrcR.h = clipped_height;

    SDL_UpdateTexture(screen_texture, &SrcR, texture+ (4*HPIXELS*TPP*SrcR.y) + SrcR.x*4, 4*HPIXELS*TPP);

    SDL_RenderCopy(renderer, screen_texture, &SrcR, NULL);

    SDL_RenderPresent(renderer);
  }
  */
  return behind;
}


int sample_size=0;
void MyAudioCallback(void*  thisAmiga,
                       Uint8* stream,
                       int    len)
{
    Amiga *amiga = (Amiga *)thisAmiga;
    //calculate number of fitting samples
    //we are copying stereo interleaved floats in LR format, therefore we must divide by 2
    //int n = len /  (2 * sizeof(float));
    amiga->paula.muxer.copy((float *)stream, sample_size); 
/*    printf("copyMono[%d]: ", n);
    for(int i=0; i<n; i++)
    {
      printf("%hhu,",stream[i]);
    }
    printf("\n");
  */ 
    sum_samples += sample_size;
}





void send_message_to_js(const char * msg)
{
    EM_ASM(
    {
        if (typeof message_handler === 'undefined')
            return;
        message_handler( "MSG_"+UTF8ToString($0) );
    }, msg );    

}

void send_message_to_js(const char * msg, long data1, long data2)
{
    EM_ASM(
    {
        if (typeof message_handler === 'undefined')
            return;
        message_handler( "MSG_"+UTF8ToString($0), $1, $2 );
    }, msg, data1, data2 );    

}



//bool paused_the_emscripten_main_loop=false;
bool already_run_the_emscripten_main_loop=false;
bool warp_mode=false;
void theListener(const void * amiga, long type,  int data1, int data2, int data3, int data4){
  if(warp_to_frame>0 && ((Amiga *)amiga)->agnus.pos.frame < warp_to_frame)
  {
    //skip automatic warp mode on disk load
  }
  else
  {
    if(warp_mode && type == MSG_DRIVE_MOTOR_ON && !((Amiga *)amiga)->inWarpMode())
    {
      ((Amiga *)amiga)->warpOn(); //setWarp(true);
    }
    else if(type == MSG_DRIVE_MOTOR_OFF && ((Amiga *)amiga)->inWarpMode())
    {
      ((Amiga *)amiga)->warpOff(); //setWarp(false);
    }
  }

  const char *message_as_string =  (const char *)MsgTypeEnum::key((MsgType)type);
  if(type == MSG_DRIVE_SELECT)
  {
  }
  else
  {
    if(log_on) printf("vAmiga message=%s, data=%u\n", message_as_string, data1);
    send_message_to_js(message_as_string, data1, data2);
  }
  if(type == MSG_VIEWPORT)
  {
    if(data1==0 && data2==0 && data3 == 0 && data4 == 0)
      return;
    if(log_on) printf("tracking MSG_VIEWPORT=%d, %d, %d, %d\n",data1, data2, data3, data4);
    hstart_min= data1;
    vstart_min= data2;
    hstop_max=  data3;
    vstop_max=  data4;
    
    hstart_min *=2;
    hstop_max *=2;

    hstart_min=hstart_min<208 ? 208:hstart_min;
    hstop_max=hstop_max>(HPIXELS+HBLANK_MAX)? (HPIXELS+HBLANK_MAX):hstop_max;

    if(vstart_min < 26) 
      vstart_min = 26;

    if(vstop_max > VPOS_MAX) 
      vstop_max = VPOS_MAX; //312
    if(ntsc && vstop_max > VPOS_MAX-PAL_EXTRA_VPIXEL )
      vstop_max = VPOS_MAX-PAL_EXTRA_VPIXEL; 
    
    if(log_on) printf("tracking MSG_VIEWPORT=%u %u %u %u\n",hstart_min, vstart_min, hstop_max, vstop_max);
    vstart_min_tracking = vstart_min;
    vstop_max_tracking = vstop_max;
    hstart_min_tracking = hstart_min;
    hstop_max_tracking = hstop_max;

    set_viewport_dimensions();
    reset_calibration=true;
  }
  else if(type == MSG_VIDEO_FORMAT)
  {
    if(log_on) printf("video format=%s\n",VideoFormatEnum::key(data1));    
    wasm_set_display(data1?"ntsc":"pal");
    request_to_reset_calibration=true;
  }
}



class vAmigaWrapper {
  public:
    Amiga *amiga;

  vAmigaWrapper()
  {
    printf("constructing vAmiga ...\n");
    this->amiga = new Amiga();

    printf("adding a listener to vAmiga message queue...\n");

    amiga->msgQueue.setListener(this->amiga, &theListener);
    amiga->defaults.setFallback(OPT_HDC_CONNECT, 0, false);


    amiga->configure(OPT_RTC_MODEL, RTC_NONE); 



    // master Volumne
    amiga->configure(OPT_AUDVOLL, 100); 
    amiga->configure(OPT_AUDVOLR, 100);

    //Volumne
    amiga->configure(OPT_AUDVOL, 0, 100); 
    amiga->configure(OPT_AUDPAN, 0, 0);


    amiga->configure(OPT_CHIP_RAM, 512);
    amiga->configure(OPT_SLOW_RAM, 512);
    amiga->configure(OPT_AGNUS_REVISION, AGNUS_OCS);

    //turn automatic hd mounting off because kick1.2 makes trouble
    amiga->configure(OPT_HDC_CONNECT,/*hd drive*/ 0, /*enable*/false);
  }
  ~vAmigaWrapper()
  {
        printf("closing wrapper");
  }

  void run()
  {
    try { amiga->isReady(); } catch(...) { 
      printf("***** put missing rom message\n");
       // amiga->msgQueue.put(ROM_MISSING); 
        EM_ASM({
          setTimeout(function() {message_handler( 'MSG_ROM_MISSING' );}, 0);
        });
    }
    
    printf("wrapper calls run on vAmiga->run() method\n");

    printf("waiting on emulator ready in javascript ...\n");
 
  }
};

EM_JS(void, console_log, (const char* str), {
  console.log(UTF8ToString(str));
});
void test_success()
{
  console_log("test_success");
  printf("2222Hello from wasm worker!\n");
  assert(!emscripten_current_thread_is_wasm_worker());

}


vAmigaWrapper *wrapper = NULL;
emscripten_wasm_worker_t worker;
void run_in_worker()
{
//  console_log("run_in_worker");
  //EM_ASM({console.log("Hello log")});
//  printf("Hello from wasm worker!\n");
//  emscripten_wasm_worker_post_function_sig(EMSCRIPTEN_WASM_WORKER_ID_PARENT,(void *) test_success, "id");
//  wasm_execute();
  thisAmiga->execute();
  executed_since_last_host_frame++;
  executed_frame_count++;
  total_executed_frame_count++;
}

void wasm_execute_in_worker()
{
  emscripten_wasm_worker_post_function_v(worker, run_in_worker); 
}


extern "C" int main(int argc, char** argv) {
  worker = emscripten_malloc_wasm_worker(/*stack size: */1024);
//  emscripten_wasm_worker_post_function_v(worker, run_in_worker); 
  printf("worker started");
  wrapper= new vAmigaWrapper();
 // initSDL(wrapper->amiga);
  wrapper->run();
  return 0;
}

extern "C" Texel * wasm_pixel_buffer()
{
  auto stable_ptr = thisAmiga->denise.pixelEngine.stablePtr();
  return stable_ptr;
}

extern "C" int wasm_get_renderer()
{ 
  return render_method;
}

extern "C" int wasm_get_render_width()
{ 
  return clipped_width;
}
extern "C" int wasm_get_render_height()
{ 
  return clipped_height;
}

extern "C" void wasm_set_target_fps(int _target_fps)
{ 
  target_fps=_target_fps;
}


extern "C" bool wasm_create_renderer(char* name)
{ 
  return true;
}



/* emulation of macos mach_absolute_time() function. */
uint64_t mach_absolute_time()
{
    uint64_t nano_now = (uint64_t)(emscripten_get_now()*1000000.0);
    //printf("emsdk_now: %lld\n", nano_now);
    return nano_now; 
}

extern "C" void wasm_key(int code, int pressed)
{
//  printf("wasm_key ( %d, %d ) \n", code, pressed);

  if(pressed==1)
  {
    wrapper->amiga->keyboard.pressKey(code);
  }
  else
  {
    wrapper->amiga->keyboard.releaseKey(code);
  }
}

extern "C" void wasm_auto_type(int code, int duration, int delay)
{
    if(log_on) printf("auto_type ( %d, %d, %d ) \n", code, duration, delay);
    wrapper->amiga->keyboard.autoType(code, MSEC(duration), MSEC(delay));
}


extern "C" void wasm_schedule_key(int code1, int code2, int pressed, int frame_delay)
{
  if(pressed==1)
  {
//    printf("scheduleKeyPress ( %d, %d, %d ) \n", code1, code2, frame_delay);
    wrapper->amiga->keyboard.pressKey(code1);
//    wrapper->amiga->keyboard.scheduleKeyPress(*new AmigaKey(code1,code2), frame_delay);
  }
  else
  {
//    printf("scheduleKeyRelease ( %d, %d, %d ) \n", code1, code2, frame_delay);
    wrapper->amiga->keyboard.releaseKey(code1);
  
  //  wrapper->amiga->keyboard.scheduleKeyRelease(*new C64Key(code1,code2), frame_delay);
  }
}





char wasm_pull_user_snapshot_file_json_result[255];


extern "C" bool wasm_has_disk(const char *drive_name)
{
  if(strcmp(drive_name,"df0") == 0)
  {
    return wrapper->amiga->df0.hasDisk();
  }
  else if (strcmp(drive_name,"dh0") == 0)
  {
    return wrapper->amiga->hd0.hasDisk();
  }
  return false;
}

extern "C" void wasm_eject_disk(const char *drive_name)
{
  if(strcmp(drive_name,"df0") == 0)
  {
    if(wrapper->amiga->df0.hasDisk())
      wrapper->amiga->df0.ejectDisk();
  }
  else if (strcmp(drive_name,"dh0") == 0)
  {
    if(wrapper->amiga->hd0.hasDisk())
    {
      wrapper->amiga->powerOff();
      wrapper->amiga->configure(OPT_HDC_CONNECT,/*hd drive*/ 0, /*enable*/false);
      wrapper->amiga->powerOn();
    }
  }
}



extern "C" char* wasm_export_disk(const char *drive_name)
{
  if(strcmp(drive_name,"df0") == 0)
  {
    if(!wrapper->amiga->df0.hasDisk())
    {
      printf("no disk in df0\n");
      sprintf(wasm_pull_user_snapshot_file_json_result, "{\"size\": 0 }");
      return wasm_pull_user_snapshot_file_json_result;
    }

    ADFFile *adf = new ADFFile(wrapper->amiga->df0);
    sprintf(wasm_pull_user_snapshot_file_json_result, "{\"address\":%lu, \"size\": %lu }",
    (unsigned long)adf->data.ptr, 
    adf->data.size
    );
  }
  else if (strcmp(drive_name,"dh0") == 0)
  {
    if(!wrapper->amiga->hd0.hasDisk())
    {
      printf("no disk in dh0\n");
      sprintf(wasm_pull_user_snapshot_file_json_result, "{\"size\": 0 }");
      return wasm_pull_user_snapshot_file_json_result;
    }

    HDFFile *hdf = new HDFFile(wrapper->amiga->hd0);
    sprintf(wasm_pull_user_snapshot_file_json_result, "{\"address\":%lu, \"size\": %lu }",
    (unsigned long)hdf->data.ptr, 
    hdf->data.size
    );
  }
  printf("return => %s\n",wasm_pull_user_snapshot_file_json_result);

  return wasm_pull_user_snapshot_file_json_result;
}

Snapshot *snapshot=NULL;
extern "C" void wasm_delete_user_snapshot()
{
//  printf("request to free user_snapshot memory\n");

  if(snapshot!=NULL)
  {
    delete snapshot;
    snapshot=NULL;
    if(log_on) printf("freed user_snapshot memory\n");
  }
}

extern "C" char* wasm_pull_user_snapshot_file()
{
  printf("wasm_pull_user_snapshot_file\n");

  wasm_delete_user_snapshot();
  snapshot = wrapper->amiga->latestUserSnapshot(); //wrapper->amiga->userSnapshot(nr);

//  printf("got snapshot %u.%u.%u\n", snapshot->getHeader()->major,snapshot->getHeader()->minor,snapshot->getHeader()->subminor );

  sprintf(wasm_pull_user_snapshot_file_json_result, "{\"address\":%lu, \"size\": %lu, \"width\": %u, \"height\":%u }",
  (unsigned long)snapshot->data.ptr, 
  snapshot->data.size,
  snapshot->getHeader()->screenshot.width,
  snapshot->getHeader()->screenshot.height
  );
  printf("return => %s\n",wasm_pull_user_snapshot_file_json_result);

  return wasm_pull_user_snapshot_file_json_result;
}

extern "C" void wasm_take_user_snapshot()
{
  wrapper->amiga->requestUserSnapshot();
}

float sound_buffer[16384 * 2];
extern "C" float* wasm_get_sound_buffer_address()
{
  return sound_buffer;
}

extern "C" unsigned wasm_copy_into_sound_buffer()
{
  auto count=wrapper->amiga->paula.muxer.stream.count();
  
  auto copied_samples=0;
  for(unsigned ipos=1024;ipos<=count;ipos+=1024)
  {
    wrapper->amiga->paula.muxer.copy(
    sound_buffer+copied_samples,
     sound_buffer+copied_samples+1024, 
     1024); 
    copied_samples+=1024*2;
//  printf("fillLevel (%lf)",wrapper->amiga->paula.muxer.stream.fillLevel());
  }
  sum_samples += copied_samples/2; 

/*  printf("copyMono[%d]: ", 16);
  for(int i=0; i<16; i++)
  {
    //printf("%hhu,",stream[i]);
    printf("%f,",sound_buffer[i]);
  }
  printf("\n"); 
*/
  return copied_samples/2;
}


extern "C" void wasm_set_warp(unsigned on)
{
  warp_mode = (on == 1);
/*
  if(wrapper->amiga->iec.isTransferring() && 
      (
        (wrapper->amiga->inWarpMode() && warp_mode == false)
        ||
        (wrapper->amiga->inWarpMode() == false && warp_mode)
      )
  )
  {
*/
  if(warp_mode == false && wrapper->amiga->inWarpMode())
  {
      wrapper->amiga->warpOff();
  }
/*  }*/
}

extern "C" void wasm_set_display(const char *name)
{
  if(log_on) printf("wasm_set_display('%s')\n",name);

  if( strcmp(name,"ntsc") == 0)
  {
    name= display_names[geometry];
    if(log_on) printf("resetting new display %s\n",name);
    if(!ntsc)
    {
      if(log_on) printf("was not yet ntsc\n");

      if(wrapper->amiga->getConfigItem(OPT_VIDEO_FORMAT)!=NTSC)
      {
        if(log_on) printf("was not yet ntsc so we have to configure it\n");
        wrapper->amiga->configure(OPT_VIDEO_FORMAT, NTSC);
      }
      target_fps=60;
      total_executed_frame_count=0;
      ntsc=true;
    }
  }
  else if( strcmp(name,"pal") == 0)
  {
    name= display_names[geometry];
    if(log_on) printf("resetting  new display %s\n",name);
    if(ntsc)
    {
      if(log_on) printf("was not yet PAL\n");
      if(wrapper->amiga->getConfigItem(OPT_VIDEO_FORMAT)!=PAL)
      {
        if(log_on) printf("was not yet PAL so we have to configure it\n");
        wrapper->amiga->configure(OPT_VIDEO_FORMAT, PAL);
      }
      target_fps=50;
      total_executed_frame_count=0;
      ntsc=false;
    }
  }
  else if( strcmp(name,"") == 0)
  {
    name= display_names[geometry];
    if(log_on) printf("reset display=%s\n",name);
  }


  if( strcmp(name,"adaptive") == 0 || 
      strcmp(name,"auto") == 0 || 
      strcmp(name,"viewport tracking") == 0)
  {
    geometry=DISPLAY_ADAPTIVE;
    wrapper->amiga->configure(OPT_VIEWPORT_TRACKING, true); 
 //   clip_offset = 0;

    xOff=252;
    yOff=26 + 6;
    clipped_width=HPIXELS-xOff;
    clipped_height=312-yOff -2*4  ;
  }
  else if( strcmp(name,"borderless") == 0)
  {
    geometry=DISPLAY_BORDERLESS;
    wrapper->amiga->configure(OPT_VIEWPORT_TRACKING, true); 
    return;
  }
  else if( strcmp(name,"narrow") == 0)
  {
    wrapper->amiga->configure(OPT_VIEWPORT_TRACKING, false); 
    geometry=DISPLAY_NARROW;
    xOff=252 + 4;
    yOff=26 +16;
    clipped_width=HPIXELS-xOff - 8;   
    //clipped_height=312-yOff -2*24 -2; 
    clipped_height=(3*clipped_width/4 +(ntsc?0:32) /*32 due to PAL?*/)/2 & 0xfffe;
    if(ntsc){clipped_height-=PAL_EXTRA_VPIXEL;}
  }
  else if( strcmp(name,"standard") == 0)
  {
    wrapper->amiga->configure(OPT_VIEWPORT_TRACKING, false); 
  
    geometry=DISPLAY_STANDARD;
    xOff=208+HBLANK_MAX;
    yOff=VBLANK_CNT +10;
    clipped_width=HPIXELS-xOff;
//    clipped_height=312-yOff -2*4  ;
//    clipped_height=(4*clipped_width/5 )/2 & 0xfffe;
    clipped_height=(3*clipped_width/4 +(ntsc?0:32) /*32 due to PAL?*/)/2 & 0xfffe;
    if(ntsc){clipped_height-=PAL_EXTRA_VPIXEL;}
  }
  else if( strcmp(name,"wider") == 0)
  {
    wrapper->amiga->configure(OPT_VIEWPORT_TRACKING, false); 
  
    geometry=DISPLAY_WIDER;
    xOff=208+ HBLANK_MAX/2;
    yOff=VBLANK_CNT + 2;
    clipped_width=(HPIXELS+HBLANK_MAX/2 )-xOff;
//    clipped_height=312-yOff -2*2;
    clipped_height=(3*clipped_width/4 +(ntsc?0:32) /*32 due to PAL?*/)/2 & 0xfffe;
    if(ntsc){clipped_height-=PAL_EXTRA_VPIXEL;}
  }
  else if( strcmp(name,"overscan") == 0)
  {
    wrapper->amiga->configure(OPT_VIEWPORT_TRACKING, false); 
  
    geometry=DISPLAY_OVERSCAN;

    xOff=208; //first pixel in dpaint iv,overscan=max 
    yOff=VBLANK_CNT; //must be even
    clipped_width=(HPIXELS+HBLANK_MAX)-xOff;
    //clipped_height=312-yOff; //must be even
    clipped_height=(3*clipped_width/4 +(ntsc?0:24) /*32 due to PAL?*/)/2 & 0xfffe;
    if(ntsc){clipped_height-=PAL_EXTRA_VPIXEL;}
  }
  if(log_on) printf("width=%d, height=%d, ratio=%f\n", clipped_width, clipped_height, (float)clipped_width/(float)clipped_height);

 /* if(render_method==RENDER_SHADER)
  {
//    SDL_SetWindowMinimumSize(window, clipped_width, clipped_height);
    SDL_RenderSetLogicalSize(renderer, clipped_width*TPP, clipped_height*2); 
    SDL_SetWindowSize(window, clipped_width*TPP, clipped_height*2);
    glViewport(0, 0, clipped_width*TPP, clipped_height*2);

    glUseProgram(basic); 
    set_texture_display_window(basic, xOff,xOff+clipped_width,yOff,yOff+clipped_height);
    glUseProgram(merge);
    set_texture_display_window(merge, xOff,xOff+clipped_width,yOff,yOff+clipped_height);
  }
  else
  {
    SDL_SetWindowMinimumSize(window, clipped_width*TPP, clipped_height);
    SDL_RenderSetLogicalSize(renderer, clipped_width*TPP, clipped_height); 
    SDL_SetWindowSize(window, clipped_width*TPP, clipped_height);
  }
*/
  EM_ASM({scaleVMCanvas()});
}

std::unique_ptr<FloppyDisk> load_disk(const char* filename, Uint8 *blob, long len)
{
  try {
    if (DMSFile::isCompatible(filename)) {
      printf("%s - Loading DMS file\n", filename);
      DMSFile dms{blob, len};
      return std::make_unique<FloppyDisk>(dms);
    }
    if (ADFFile::isCompatible(filename)) {
      printf("%s - Loading ADF file\n", filename);
      ADFFile adf{blob, len};
      return std::make_unique<FloppyDisk>(adf);
    }
    if (EXEFile::isCompatible(filename)) {
      printf("%s - Loading EXE file\n", filename);
      EXEFile exe{blob, len};
      return std::make_unique<FloppyDisk>(exe);
    }
  } catch (const VAError& e) {
    printf("Error loading %s - %s\n", filename, e.what());
  }
  return {};
}

extern "C" const char* wasm_loadFile(char* name, Uint8 *blob, long len)
{
  printf("load file=%s len=%ld, header bytes= %x, %x, %x\n", name, len, blob[0],blob[1],blob[2]);
  filename=name;
  if(wrapper == NULL)
  {
    return "";
  }
  if (auto disk = load_disk(name, blob, len)) {
    //wrapper->amiga->paula.diskController.insertDisk(std::move(disk), 0, (Cycle)SEC(1.8));
    wrapper->amiga->df0.swapDisk(std::move(disk));
    return "";
  }

  if (HDFFile::isCompatible(filename)) {
    printf("is hdf\n");
    wrapper->amiga->powerOff();
    //HDFFile hdf{blob, len};  
    HDFFile *hdf = new HDFFile(blob, len);
    wrapper->amiga->configure(OPT_HDC_CONNECT,/*hd drive*/ 0, /*enable*/true);
    wrapper->amiga->hd0.init(*hdf);
    delete hdf;
    wrapper->amiga->powerOn();
    return "";
  }

  bool file_still_unprocessed=true;
  if (Snapshot::isCompatible(filename) && util::extractSuffix(filename)!="rom")
  {
    try
    {
      if(log_on) printf("try to build Snapshot\n");
      Snapshot *file = new Snapshot(blob, len);      
      printf("isSnapshot\n");
      wrapper->amiga->loadSnapshot(*file);
      file_still_unprocessed=false;
      delete file;
//      wasm_set_display(wrapper->amiga->agnus.isNTSC()?"ntsc":"pal");

/*      if(geometry==DISPLAY_BORDERLESS || geometry == DISPLAY_ADAPTIVE)
      {//it must determine the viewport again, i.e. we need a new message for calibration
        //enforce this by calling
        wrapper->amiga->configure(OPT_VIEWPORT_TRACKING, true); 
      }
*/
      printf("run snapshot at %d Hz, isPAL=%d\n", target_fps, !ntsc);
    }
    catch(VAError &exception) {
      ErrorCode ec=exception.data;
      printf("%s\n", ErrorCodeEnum::key(ec));
    }
  }

  if(file_still_unprocessed && util::extractSuffix(filename)=="rom_file")
  {
    bool wasRunnable = true;
    try { wrapper->amiga->isReady(); } catch(...) { wasRunnable=false; }

    RomFile *rom = NULL;
    try
    {
      printf("try to build RomFile\n");
      rom = new RomFile(blob, len);
    }
    catch(VAError &exception) {
      printf("Failed to read ROM image file %s\n", name);
      ErrorCode ec=exception.data;
      printf("%s\n", ErrorCodeEnum::key(ec));
      return "";
    }

    if(wrapper->amiga->isPoweredOn())
    {
      wrapper->amiga->powerOff();
    }
//    wrapper->amiga->suspend();
    try { 
      wrapper->amiga->mem.loadRom(*rom); 
      
      printf("Loaded ROM image %s. %s\n", name, wrapper->amiga->mem.romTitle());
       
/*      wrapper->amiga->configure(OPT_HDC_CONNECT,
        //hd drive
        0, 
        //enable if not AROS
        strcmp(wrapper->amiga->mem.romTitle(),"AROS Kickstart replacement")!=0
      );
*/
    }  
    catch(VAError &exception) { 
      printf("Failed to flash ROM image %s.\n", name);
      ErrorCode ec=exception.data;
      printf("%s\n", ErrorCodeEnum::key(ec));
      return "";
    }
    const char *rom_type="rom";
    try
    {
        bool is_ready_now = true;
        try { wrapper->amiga->isReady(); } catch(...) { is_ready_now=false; }

        if (!wasRunnable && is_ready_now)
        {
          printf("was not runnable is ready now (rom)\n");
          wrapper->amiga->powerOn();
        
          //wrapper->amiga->putMessage(MSG_READY_TO_RUN);
          const char* ready_msg= "READY_TO_RUN";
          printf("sending ready message %s.\n", ready_msg);
          send_message_to_js(ready_msg);    
        }

        delete rom;
        wrapper->amiga->powerOn();
//        wrapper->amiga->resume();
    }    
    catch(VAError &exception) { 
      ErrorCode ec=exception.data;
      printf("%s\n", ErrorCodeEnum::key(ec));
    } 

    return rom_type;    
  }
  
  if(file_still_unprocessed && util::extractSuffix(filename)=="rom_ext_file")
  {
    bool wasRunnable = true;
    try { wrapper->amiga->isReady(); } catch(...) { wasRunnable=false; }

    ExtendedRomFile *rom = NULL;
    try
    {
      printf("try to build ExtendedRomFile\n");
      rom = new ExtendedRomFile(blob, len);
    }
    catch(VAError &exception) {
      printf("Failed to read ROM_EXT image file %s\n", name);
      ErrorCode ec=exception.data;
      printf("%s\n", ErrorCodeEnum::key(ec));
      return "";
    }

    if(wrapper->amiga->isPoweredOn())
    {
      wrapper->amiga->powerOff();
    }
    //wrapper->amiga->suspend();
    try { 
      wrapper->amiga->mem.loadExt(*rom); 
      
      printf("Loaded ROM_EXT image %s.\n", name);
      
    }  
    catch(VAError &exception) { 
      printf("Failed to flash ROM_EXT image %s.\n", name);
      ErrorCode ec=exception.data;
      printf("%s\n", ErrorCodeEnum::key(ec));
    }
  

    bool is_ready_now = true;
    try { wrapper->amiga->isReady(); } catch(...) { is_ready_now=false; }

    if (!wasRunnable && is_ready_now)
    {
      printf("was not runnable is ready now (romext)\n");
      wrapper->amiga->powerOn();

       //wrapper->amiga->putMessage(MSG_READY_TO_RUN);
      const char* ready_msg= "READY_TO_RUN";
      printf("sending ready message %s.\n", ready_msg);
      send_message_to_js(ready_msg);    
    }

    const char *rom_type="rom_ext";
    delete rom;
    //wrapper->amiga->resume();
    wrapper->amiga->powerOn();
    return rom_type;    
  }

  return "";
}


extern "C" void wasm_reset()
{
  wrapper->amiga->reset(true);
}


extern "C" void wasm_halt()
{
  printf("wasm_halt\n");
  wrapper->amiga->pause();

//  printf("emscripten_pause_main_loop() at MSG_PAUSE\n");
//  paused_the_emscripten_main_loop=true;
  //emscripten_pause_main_loop();
//  printf("after emscripten_set_main_loop_arg() at MSG_RUN\n");

}

extern "C" void wasm_run()
{
  if(log_on) printf("wasm_run\n");
  
  if(log_on) printf("is running = %u\n",wrapper->amiga->isRunning());

  wrapper->amiga->run();
  thisAmiga=wrapper->amiga;
/*
  if(paused_the_emscripten_main_loop || already_run_the_emscripten_main_loop)
  {
    if(log_on) printf("emscripten_resume_main_loop at MSG_RUN %u, %u\n", paused_the_emscripten_main_loop, already_run_the_emscripten_main_loop);
//    emscripten_resume_main_loop();
  }
  else
  {
    if(log_on) printf("emscripten_set_main_loop_arg() at MSG_RUN %u, %u\n", paused_the_emscripten_main_loop, already_run_the_emscripten_main_loop);
    already_run_the_emscripten_main_loop=true;
    thisAmiga=wrapper->amiga;
    //emscripten_set_main_loop_arg(draw_one_frame_into_SDL, (void *)wrapper->amiga, 0, 1);
    if(log_on) printf("after emscripten_set_main_loop_arg() at MSG_RUN\n");
  }
*/
}


extern "C" void wasm_mouse(int port, int x, int y)
{
  //printf("wasm_mouse port%d x=%d, y=%d\n", port, x, y);
  if(port==1)
    wrapper->amiga->controlPort1.mouse.setDxDy(x,y);
  else if(port==2)
    wrapper->amiga->controlPort2.mouse.setDxDy(x,y);
}

extern "C" void wasm_mouse_button(int port, int button_id, int pressed)
{ 
  if(port==1)
  {
    if(button_id==1)
      wrapper->amiga->controlPort1.mouse.setLeftButton(pressed==1);
    else if(button_id==3)
      wrapper->amiga->controlPort1.mouse.setRightButton(pressed==1);
  }
  else if(port==2)
  {
    if(button_id==1)
      wrapper->amiga->controlPort2.mouse.setLeftButton(pressed==1);
    else if(button_id==3)
      wrapper->amiga->controlPort2.mouse.setRightButton(pressed==1);
  }
}

extern "C" void wasm_joystick(char* port_plus_event)
{
//    printf("wasm_joystick event=%s\n", port_plus_event);
  /*
  from javascript
    // port, o == oben, r == rechts, ..., f == feuer
    //states = '1', '1o', '1or', '1r', '1ur', '1u', '1ul', '1l', '1ol' 
    //states mit feuer = '1f', '1of', '1orf', '1rf', '1urf', ...
  */
/*
outgoing
PULL_UP
PRESS_FIRE

RELEASE_X
RELEASE_Y
RELEASE_XY
RELEASE_FIRE
*/
  char joyport = port_plus_event[0];
  char* event  = port_plus_event+1;

  GamePadAction code;
  if( strcmp(event,"PULL_UP") == 0)
  {
    code = PULL_UP;
  }
  else if( strcmp(event,"PULL_LEFT") == 0)
  {
    code = PULL_LEFT;
  }
  else if( strcmp(event,"PULL_DOWN") == 0)
  {
    code = PULL_DOWN;
  }
  else if( strcmp(event,"PULL_RIGHT") == 0)
  {
    code = PULL_RIGHT;
  }
  else if( strcmp(event,"PRESS_FIRE") == 0)
  {
    code = PRESS_FIRE;
  }
  else if( strcmp(event,"RELEASE_XY") == 0)
  {
    code = RELEASE_XY;
  }
  else if( strcmp(event,"RELEASE_X") == 0)
  {
    code = RELEASE_X;
  }
  else if( strcmp(event,"RELEASE_Y") == 0)
  {
    code = RELEASE_Y;
  }
  else if( strcmp(event,"RELEASE_FIRE") == 0)
  {
    code = RELEASE_FIRE;
  }


  else
  {
    return;    
  }

  if(joyport == '1')
  {
    wrapper->amiga->controlPort1.joystick.trigger(code);
  }
  else if(joyport == '2')
  {
    wrapper->amiga->controlPort2.joystick.trigger(code);
  }

}

char buffer[50];
extern "C" char* wasm_sprite_info()
{
  if(!wrapper->amiga->inDebugMode())
  {
    wrapper->amiga->debugOn();
  }
//   wrapper->amiga->setInspectionTarget(INSPECTION_DENISE, MSEC(250));
//   wrapper->amiga->denise.debugger.recordSprite(0);

   Denise *denise = &(wrapper->amiga->denise);
   auto spriteinfo0 = denise->debugger.getSpriteInfo(0);
   auto spriteinfo1 = denise->debugger.getSpriteInfo(1);
   auto spriteinfo2 = denise->debugger.getSpriteInfo(2);
   auto spriteinfo3 = denise->debugger.getSpriteInfo(3);
   auto spriteinfo4 = denise->debugger.getSpriteInfo(4);
   auto spriteinfo5 = denise->debugger.getSpriteInfo(5);
   auto spriteinfo6 = denise->debugger.getSpriteInfo(6);
   auto spriteinfo7 = denise->debugger.getSpriteInfo(7);


   sprintf(buffer, "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu", 
     spriteinfo0.hstrt*2,
     spriteinfo0.vstrt, 
     spriteinfo1.hstrt*2,
     spriteinfo1.vstrt, 
     spriteinfo2.hstrt*2,
     spriteinfo2.vstrt, 
     spriteinfo3.hstrt*2,
     spriteinfo3.vstrt, 
     spriteinfo4.hstrt*2,
     spriteinfo4.vstrt, 
     spriteinfo5.hstrt*2,
     spriteinfo5.vstrt, 
     spriteinfo6.hstrt*2,
     spriteinfo6.vstrt, 
     spriteinfo7.hstrt*2,
     spriteinfo7.vstrt
     );  

   return buffer;
}

extern "C" void wasm_set_sid_model(unsigned SID_Model)
{
/*
  bool wasRunning=false;
  if(wrapper->amiga->isRunning()){
    wasRunning= true;
    wrapper->amiga->pause();
  }
  if(SID_Model == 6581)
  {
    wrapper->amiga->configure(OPT_SID_REVISION, MOS_6581);
  }
  else if(SID_Model == 8580)
  {
    wrapper->amiga->configure(OPT_SID_REVISION, MOS_8580);  
  }
  if(wasRunning)
  {
    wrapper->amiga->run();
  }
  */
}

extern "C" void wasm_cut_layers(unsigned cut_layers)
{
  wrapper->amiga->configure(OPT_HIDDEN_LAYER_ALPHA,255);
//  wrapper->amiga->configure(OPT_HIDDEN_SPRITES, 0x100 | (SPR0|SPR1|SPR2|SPR3|SPR4|SPR5|SPR6|SPR7)); 
  wrapper->amiga->configure(OPT_HIDDEN_LAYERS, cut_layers); 
}



char json_result[1024];
extern "C" const char* wasm_rom_info()
{

  sprintf(json_result, "{\"hasRom\":\"%s\",\"hasExt\":\"%s\", \"romTitle\":\"%s\", \"romVersion\":\"%s\", \"romReleased\":\"%s\", \"romModel\":\"%s\", \"extTitle\":\"%s\", \"extVersion\":\"%s\", \"extReleased\":\"%s\", \"extModel\":\"%s\" }",
    wrapper->amiga->mem.hasRom()?"true":"false",
    wrapper->amiga->mem.hasExt()?"true":"false",
    wrapper->amiga->mem.romTitle(),
    wrapper->amiga->mem.romVersion(),
    wrapper->amiga->mem.romReleased(),
    wrapper->amiga->mem.romModel(),
    wrapper->amiga->mem.extTitle(),
    wrapper->amiga->mem.extVersion(),
    wrapper->amiga->mem.extReleased(),
    wrapper->amiga->mem.extModel()
  );

/*
  printf("%s, %s, %s, %s\n",      wrapper->amiga->mem.romTitle(),
    wrapper->amiga->mem.romVersion(),
    wrapper->amiga->mem.romReleased(),
    ""
//    wrapper->amiga->mem.romModel()
  );
*/
  return json_result;
}

extern "C" const char* wasm_get_core_version()
{
  sprintf(json_result, "%s",
    wrapper->amiga->version().c_str() 
  );

  return json_result;
}



extern "C" void wasm_set_color_palette(char* palette)
{

  if( strcmp(palette,"color") == 0)
  {
    wrapper->amiga->configure(OPT_PALETTE, PALETTE_COLOR);
  }
  else if( strcmp(palette,"black white") == 0)
  { 
    wrapper->amiga->configure(OPT_PALETTE, PALETTE_BLACK_WHITE); 
  }
  else if( strcmp(palette,"paper white") == 0)
  { 
    wrapper->amiga->configure(OPT_PALETTE, PALETTE_PAPER_WHITE); 
  }
  else if( strcmp(palette,"green") == 0)
  { 
    wrapper->amiga->configure(OPT_PALETTE, PALETTE_GREEN); 
  }
  else if( strcmp(palette,"amber") == 0)
  { 
    wrapper->amiga->configure(OPT_PALETTE, PALETTE_AMBER); 
  }
  else if( strcmp(palette,"sepia") == 0)
  { 
    wrapper->amiga->configure(OPT_PALETTE, PALETTE_SEPIA); 
  }

}


extern "C" u64 wasm_get_cpu_cycles()
{
  return wrapper->amiga->cpu.getClock();
}

extern "C" u8 wasm_peek(u16 addr)
{
  //return wrapper->amiga->mem.spypeek(addr);
  return 0;
}


extern "C" void wasm_write_string_to_ser(char* chars_to_send)
{
//  wrapper->amiga->write_string_to_ser(chars_to_send);
}

//const char chars_to_send[] = "HELLOMYWORLD!";
extern "C" void wasm_poke(u16 addr, u8 value)
{
    //wrapper->amiga->mem.poke(addr, value);
}

/*
char wasm_translate_json[255];
extern "C" char* wasm_translate(char c)
{
  auto key = C64Key::translate(c);
  sprintf(wasm_translate_json, "{\"size\": "+key.+" }");
  sprintf( wasm_translate_json);
  
  return wasm_translate_json;
}
*/


char config_result[512];
extern "C" const char* wasm_power_on(unsigned power_on)
{
  try{
    bool was_powered_on=wrapper->amiga->isPoweredOn();
    if(power_on == 1 && !was_powered_on)
    {
        wrapper->amiga->powerOn();
    }
    else if(power_on == 0 && was_powered_on)
    {
        wrapper->amiga->powerOff();
    }
  }  
  catch(VAError &exception) {   
    sprintf(config_result,"%s", exception.what());
  }
  return config_result; 
}


extern "C" void wasm_set_sample_rate(unsigned sample_rate)
{
    printf("set paula.muxer to freq= %d\n", sample_rate);
    wrapper->amiga->paula.muxer.setSampleRate(sample_rate);
    printf("paula.muxer.getSampleRate()==%f\n", wrapper->amiga->paula.muxer.getSampleRate());
}



extern "C" i64 wasm_get_config_item(char* item_name)
{
  //if(wrapper->amiga->getConfigItem(OPT_VIDEO_FORMAT)!=NTSC)
  return wrapper->amiga->getConfigItem(util::parseEnum <OptionEnum>(std::string(item_name)));
}

extern "C" const char* wasm_configure(char* option, char* _value)
{
  sprintf(config_result,""); 
  auto value = std::string(_value);
  if(log_on) printf("wasm_configure %s = %s\n", option, value.c_str());

  if(strcmp(option,"warp_to_frame") == 0 )
  {
    warp_to_frame= util::parseNum(value);
    wrapper->amiga->warpOn();
    return config_result;
  }
  if(strcmp(option,"log_on") == 0 )
  {
    log_on= util::parseBool(value);
    return config_result;
  }


  bool was_powered_on=wrapper->amiga->isPoweredOn();

  bool must_be_off= strcmp(option,"AGNUS_REVISION") == 0 || 
                    strcmp(option,"DENISE_REVISION") == 0 ||
                    strcmp(option,"CHIP_RAM") == 0 ||
                    strcmp(option,"SLOW_RAM") == 0 ||
                    strcmp(option,"FAST_RAM") == 0 ||
                    strcmp(option,"CPU_REVISION") == 0;
 
  if(was_powered_on && must_be_off)
  {
      wrapper->amiga->powerOff();
  }

  try{
    if( strcmp(option,"AGNUS_REVISION") == 0)
    {
      wrapper->amiga->configure(util::parseEnum <OptionEnum>(std::string(option)), util::parseEnum <AgnusRevisionEnum>(value)); 
    }
    else if( strcmp(option,"DENISE_REVISION") == 0)
    {
      wrapper->amiga->configure(util::parseEnum <OptionEnum>(std::string(option)), util::parseEnum <DeniseRevisionEnum>(value));
    }
    else if ( strcmp(option,"BLITTER_ACCURACY") == 0 ||
              strcmp(option,"DRIVE_SPEED") == 0  ||
              strcmp(option,"CHIP_RAM") == 0  ||
              strcmp(option,"SLOW_RAM") == 0  ||
              strcmp(option,"FAST_RAM") == 0  ||
              strcmp(option,"CPU_OVERCLOCKING") == 0 ||
              strcmp(option,"CPU_REVISION") == 0
    )
    {
      wrapper->amiga->configure(util::parseEnum <OptionEnum>(std::string(option)), util::parseNum(value));
    }
    else
    {
      wrapper->amiga->configure(util::parseEnum <OptionEnum>(std::string(option)), util::parseBool(value)); 
    }

    if(was_powered_on && must_be_off)
    {
        wrapper->amiga->powerOn();
    }
  }
  catch(VAError &exception) {    
//    ErrorCode ec=exception.data;
//    sprintf(config_result,"%s", ErrorCodeEnum::key(ec));
    sprintf(config_result,"%s", exception.what());
  }
  return config_result; 
}
extern "C" void wasm_print_error(unsigned exception_ptr)
{
  if(exception_ptr!=0)
  {
    string s= std::string(reinterpret_cast<std::exception *>(exception_ptr)->what());
    printf("uncaught exception %u: %s\n",exception_ptr, s.c_str());
  }
}


