#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include <X11/X.h>
#include <X11/Xlib.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glu.h>

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

#include "glxwin.h"

static Display                 *dpy = NULL;
static Window                  root;
static GLint                   att[] = { GLX_RGBA, GLX_DEPTH_SIZE, 24, GLX_DOUBLEBUFFER, None };
static XVisualInfo             *vi = NULL;
static Colormap                cmap;
static XSetWindowAttributes    swa;
static Window                  win;
static GLXContext              glc;
static XWindowAttributes       gwa;
static XEvent                  xev;

static GLuint gltexture = 0;
static int    spawn_new_window = 0;

glxwin_shm_t *glx_shm = NULL;

static int screen_width  = 512;
static int screen_height = 512;

extern GtkWidget *evbox;
extern unsigned int gtk_draw_area_width;
extern unsigned int gtk_draw_area_height;
//************************************************************************
static glxwin_shm_t *open_shm(void)
{
	int shmId;
	glxwin_shm_t *vaddr;
	struct shmid_ds ds;

	shmId = shmget( IPC_PRIVATE, sizeof(struct glxwin_shm_t), IPC_CREAT | S_IRWXU | S_IRWXG );

	if ( shmId == -1 )
	{
	   perror("Error: GLX shmget Failed:");
		return NULL;
	}
	printf("Created ShmID: %i \n", shmId );

	vaddr = (glxwin_shm_t*)shmat( shmId, NULL, 0);

	if ( vaddr == (glxwin_shm_t*)-1 )
	{
	   perror("Error: GLX shmat Failed:");
		return NULL;
	}
	memset( vaddr, 0, sizeof(struct glxwin_shm_t));

	if ( shmctl( shmId, IPC_RMID, &ds ) != 0 )
	{
	   perror("Error: GLX shmctl IPC_RMID Failed:");
	}

	sem_init( &vaddr->sem, 1, 1 );

	return vaddr;
}
//************************************************************************
static void genTextures(void)
{
	int ipolate = 1;

	glEnable(GL_TEXTURE_2D);
   glGenTextures(1, &gltexture);

	glBindTexture(GL_TEXTURE_2D, gltexture);

	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,ipolate?GL_LINEAR:GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,ipolate?GL_LINEAR:GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP);

}
//************************************************************************
static int open_window(void)
{

	dpy = XOpenDisplay(NULL);
	 
	if (dpy == NULL) {
		printf("\n\tcannot connect to X server\n\n");
	   exit(0);
	}
	root = DefaultRootWindow(dpy);

	vi = glXChooseVisual(dpy, 0, att);

	if (vi == NULL) 
	{
		printf("\n\tno appropriate visual found\n\n");
	   exit(0);
	} 
	else {
		printf("\n\tvisual %p selected\n", (void *)vi->visualid); /* %p creates hexadecimal output like in glxinfo */
	}

	cmap = XCreateColormap(dpy, root, vi->visual, AllocNone);

	swa.colormap   = cmap;
	swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | StructureNotifyMask;

	win = XCreateWindow(dpy, root, 0, 0, screen_width, screen_height, 0, 
			vi->depth, InputOutput, vi->visual, 
				CWColormap | CWEventMask, &swa);

	XMapWindow(dpy, win);

	XStoreName(dpy, win, "FCEUX VIEWPORT");

	glc = glXCreateContext(dpy, vi, NULL, GL_TRUE);

	if ( glc == NULL )
	{
		printf("Error: glXCreateContext Failed\n");
	}
	glXMakeCurrent(dpy, win, glc);

	genTextures();
	glDisable(GL_DEPTH_TEST);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);	// Background color to black.
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	// In a double buffered setup with page flipping, be sure to clear both buffers.
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	return 0;
}
//************************************************************************
static void print_pixbuf(void)
{
   for (int x=0; x<256; x++)
	{
		for (int y=0; y<256; y++)
		{
			printf("(%i,%i) = %08X \n", x, y, glx_shm->pixbuf[x*256+y] );
		}
	}
}
//************************************************************************
static void render_image(void)
{
	int l=0, r=GLX_NES_WIDTH;
	int t=0, b=GLX_NES_HEIGHT;

	float xscale = (float)screen_width  / (float)GLX_NES_WIDTH;
	float yscale = (float)screen_height / (float)GLX_NES_HEIGHT;
	if (xscale < yscale )
	{
		yscale = xscale;
	}
	else 
	{
		xscale = yscale;
	}
	int rw=(int)((r-l)*xscale);
	int rh=(int)((b-t)*yscale);
	int sx=(screen_width-rw)/2;   
	int sy=(screen_height-rh)/2;   

	glXMakeCurrent(dpy, win, glc);
	//printf("Viewport: (%i,%i)   %i    %i    %i    %i \n", 
	//		screen_width, screen_height, sx, sy, rw, rh );
	glViewport(sx, sy, rw, rh);
	//glViewport( 0, 0, screen_width, screen_height);

	glLoadIdentity();
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glOrtho( -1.0,  1.0,  -1.0,  1.0,  -1.0,  1.0);

	glDisable(GL_DEPTH_TEST);
	glClearColor( 0.0, 0.0f, 0.0f, 0.0f);	// Background color to black.
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glEnable(GL_LINE_SMOOTH);
	glEnable(GL_TEXTURE_2D);
	//glBindTexture(GL_TEXTURE_2D, gltexture);
	glBindTexture(GL_TEXTURE_2D, gltexture);

	//print_pixbuf();
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 256, 0,
					GL_RGBA, GL_UNSIGNED_BYTE, glx_shm->pixbuf );

	glBegin(GL_QUADS);
	glTexCoord2f(1.0f*l/256, 1.0f*b/256); // Bottom left of picture.
	glVertex2f(-1.0f, -1.0f);	// Bottom left of target.

	glTexCoord2f(1.0f*r/256, 1.0f*b/256);// Bottom right of picture.
	glVertex2f( 1.0f, -1.0f);	// Bottom right of target.

	glTexCoord2f(1.0f*r/256, 1.0f*t/256); // Top right of our picture.
	glVertex2f( 1.0f,  1.0f);	// Top right of target.

	glTexCoord2f(1.0f*l/256, 1.0f*t/256);  // Top left of our picture.
	glVertex2f(-1.0f,  1.0f);	// Top left of target.
	glEnd();

	glDisable(GL_TEXTURE_2D);

	//glColor4f( 1.0, 1.0, 1.0, 1.0 );
	//glLineWidth(5.0);
	//glBegin(GL_LINES);
	//glVertex2f(-1.0f, -1.0f);	// Bottom left of target.
	//glVertex2f( 1.0f,  1.0f);	// Top right of target.
	//glEnd();

	glFlush();

	glXSwapBuffers( dpy, win );
}
//************************************************************************
static int mainWindowLoop(void)
{

	while( glx_shm->run ) 
	{
	 	while ( XPending( dpy ) )
		{
			XNextEvent(dpy, &xev);

	   	if (xev.type == Expose) 
			{
	   		XGetWindowAttributes(dpy, win, &gwa);
	   	   //glViewport(0, 0, gwa.width, gwa.height);
	   		//DrawAQuad(); 
	   	   glXSwapBuffers(dpy, win);
				printf("Expose\n");
	   	}
			else if (xev.type == ConfigureNotify) 
			{
	   		//XGetWindowAttributes(dpy, win, &gwa);

				screen_width  = xev.xconfigure.width;
				screen_height = xev.xconfigure.height;

				//if (gltexture) {
				//	glDeleteTextures(1, &gltexture);
				//}
				//gltexture=0;

				//genTextures();

				printf("Resize Request: (%i,%i)\n", screen_width, screen_height );
				render_image();
	   	   //glViewport(0, 0, gwa.width, gwa.height);
	   		//DrawAQuad(); 
	   	   //glXSwapBuffers(dpy, win);
	   	}
			else if (xev.type == KeyPress) 
			{
				printf("Key press: %i  %08X  \n", xev.xkey.keycode, xev.xkey.state );

			}
			else if (xev.type == DestroyNotify) 
			{
				printf("DestroyNotify\n");
				glx_shm->run = 0;
	   	   //glXMakeCurrent(dpy, None, NULL);
	 			//glXDestroyContext(dpy, glc);
	 			//XDestroyWindow(dpy, win);
	 			//XCloseDisplay(dpy);
	 			//exit(0);
	   	}
	   }

		if ( glx_shm->blit_count != glx_shm->render_count )
		{
			render_image();
			glx_shm->render_count++;
		}

		usleep(10000);
	}

	glXMakeCurrent(dpy, None, NULL);
	glXDestroyContext(dpy, glc);
	XDestroyWindow(dpy, win);
	XCloseDisplay(dpy);
	exit(0);
   return 0;
}
//************************************************************************
int  spawn_glxwin( int flags )
{
	int pid;

	glx_shm = open_shm();

	if ( !spawn_new_window )
	{
		return 0;
	}
	pid = fork();

	if ( pid == 0 )
	{
		// Child Process
		glx_shm->run = 1;
		glx_shm->pid = getpid();

		printf("Child Process Running: %i\n", glx_shm->pid );

		open_window();

		mainWindowLoop();

		exit(0);
	}
	else if ( pid > 0 )
	{  // Parent Process

	}
	else
	{
		// Error
		printf("Error: Failed to Fork GLX Window Process\n");
	}

   return pid;
}
//************************************************************************
int  init_gtk3_GLXContext( void )
{
	XWindowAttributes xattrb;

	GdkWindow *gdkWin = gtk_widget_get_window(evbox);

	if ( gdkWin == NULL )
	{
		printf("Error: Failed to obtain gdkWindow Handle for evbox widget\n");
		return -1;
	}
	win = GDK_WINDOW_XID( gdkWin );

	root = GDK_ROOT_WINDOW();

	dpy = gdk_x11_get_default_xdisplay();

	if ( dpy == NULL )
	{
		printf("Error: Failed to obtain X Display Handle for evbox widget\n");
		return -1;
	}

	if ( XGetWindowAttributes( dpy, win, &xattrb ) == 0 )
	{
		printf("Error: XGetWindowAttributes failed\n");
		return -1;
	}
	printf("XWinLocation: (%i,%i) \n", xattrb.x, xattrb.y );
	printf("XWinSize: (%i x %i) \n", xattrb.width, xattrb.height );
	printf("XWinDepth: %i \n", xattrb.depth );
	printf("XWinVisual: %p \n", xattrb.visual );

	vi = glXChooseVisual(dpy, 0, att);

	if (vi == NULL) 
	{
		printf("\n\tno appropriate visual found\n\n");
	   exit(0);
	} 
	else {
		printf("\n\tvisual %p selected\n", (void *)vi->visualid); /* %p creates hexadecimal output like in glxinfo */
	}

	glc = glXCreateContext(dpy, vi, NULL, GL_TRUE);

	if ( glc == NULL )
	{
		printf("Error: glXCreateContext Failed\n");
	}
	glXMakeCurrent(dpy, win, glc);

	genTextures();
	glDisable(GL_DEPTH_TEST);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);	// Background color to black.
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	// In a double buffered setup with page flipping, be sure to clear both buffers.
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	return 0;
}
//************************************************************************
int  gtk3_glx_render(void)
{
	screen_width  = gtk_draw_area_width;
	screen_height = gtk_draw_area_height;

	render_image();

	return 0;
}
//************************************************************************
