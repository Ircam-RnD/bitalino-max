/**
 *
 * @file bitalino-max.cpp
 * @author joseph.larralde@ircam.fr (osx version)
 * @author riccardo.borghesi@ircam.fr (windows version)
 *
 * @brief max interface object for the BITalino API
 *
 * Copyright (C) 2015 by IRCAM – Centre Pompidou, Paris, France.
 * All rights reserved.

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
  
*/

#include "bitalino.h"
#include "ext.h"
#include "ext_obex.h"
#include <stdio.h>
#include "ext_systhread.h"

// Global var prevents other objects to interfere with device currently in use.
// First object to call to start connects to BITalino and has the exclusive connection until
bool bitalino_busy;

typedef struct _bitalino {
	t_object p_ob;
    
	BITalino            *bitalinoDev;
	t_systhread         systhread;			// thread reference
	t_systhread_mutex	mutex;				// mutual exclusion lock for threadsafety
	int                 systhread_cancel;	// thread cancel flag
	void				*qelem;				// for message passing between threads
    int                 sleeptime;
    
    BITalino::VFrame    *frames;
    BITalino::VFrame    *local_frames;
    
    const char          *messages_out[6];
    void                *m_poll;
    double              poll_interval;
	void                *p_outlet;
} t_bitalino;


void bitalino_bang(t_bitalino *x);
void *bitalino_get(t_bitalino *x);  // threaded function
void bitalino_qfn(t_bitalino *x);   // function writing frames to thread-safe local frames
void bitalino_clock(t_bitalino *x);
void bitalino_start(t_bitalino *x, t_symbol *s, long argc, t_atom *argv);
void bitalino_stop(t_bitalino *x);
void bitalino_disconnect(t_bitalino *x);
void bitalino_poll(t_bitalino *x, long n);
void bitalino_nopoll(t_bitalino *x);
void bitalino_assist(t_bitalino *x, void *b, long m, long a, char *s);
void *bitalino_new();
void bitalino_free(t_bitalino *x);

t_class *bitalino_class;


//--------------------------------------------------------------------------

// main method called only once in a Max session
int C74_EXPORT main(void)
{
	t_class *c;
    
	c = class_new("bitalino", (method)bitalino_new, (method)bitalino_free, sizeof(t_bitalino), 0L, A_DEFLONG, 0);
	
    class_addmethod(c, (method)bitalino_bang,		"bang",		0);	// the method it uses when it gets a bang in the left inlet
    class_addmethod(c, (method)bitalino_start,      "start",    A_GIMME, 0);
    class_addmethod(c, (method)bitalino_stop,       "stop",     0);
    class_addmethod(c, (method)bitalino_poll,       "poll",     A_LONG, 0);
    class_addmethod(c, (method)bitalino_nopoll,     "nopoll",   0);
    class_addmethod(c, (method)bitalino_assist,     "assist",   A_CANT, 0);	// (optional) assistance method needs to be declared like this
	class_addmethod(c, (method)bitalino_disconnect, "disconnect", 0);

	class_register(CLASS_BOX, c);
	bitalino_class = c;
    bitalino_busy = false;
    
	post("bitalino object loaded ...");
	return 0;
}


//--------------------------------------------------------------------------

void *bitalino_new()
{
	t_bitalino *x;

	x = (t_bitalino *)object_alloc(bitalino_class);
  x->messages_out[0] = "/EMG";
  x->messages_out[1] = "/EDA";
  x->messages_out[2] = "/ECG";
  x->messages_out[3] = "/ACCEL";
  x->messages_out[4] = "/LUX";
  x->messages_out[5] = "/UNKNOWN";
    
	x->p_outlet = outlet_new(x, NULL);
    
	x->bitalinoDev = NULL;
  x->qelem = qelem_new(x,(method)bitalino_qfn);
	x->systhread = NULL;
	systhread_mutex_new(&x->mutex,0);
    
  x->sleeptime = 10;
  x->frames = new BITalino::VFrame(20);
  
  x->m_poll = clock_new((t_object *)x, (method)bitalino_clock);
  x->local_frames = new BITalino::VFrame(20);
    
	return(x);
}

void bitalino_free(t_bitalino *x)
{
  // stop thread
  bitalino_stop(x);
    
  if (x->qelem)
    qelem_free(x->qelem);

  // free out mutex
  if (x->mutex)
    systhread_mutex_free(x->mutex);

  object_free(x->m_poll);
  delete(x->frames);
  delete(x->local_frames);
  
  if (x->bitalinoDev != NULL)
    delete x->bitalinoDev;
}

//--------------------------------------------------------------------------

void bitalino_assist(t_bitalino *x, void *b, long m, long a, char *s)
{
	if (m == ASSIST_OUTLET)
		sprintf(s,"OSC-style BITalino channels messages");
	else {
		switch (a) {	
		case 0:
			sprintf(s,"start, poll <interval>, nopoll");
			break;
		}
	}
}

void *bitalino_get(t_bitalino *x)
{
  try 
  {
    if (x->bitalinoDev == NULL)
    {
#ifdef WIN32
      std::string bitalinoaddress = "COM5";
      
      post("BITalino: looking for device");
      
      BITalino::VDevInfo devs = BITalino::find();
      for (int i = 0; i < devs.size(); i++)
        if(_memicmp(devs[i].name.c_str(), "bitalino", 8) == 0)
          bitalinoaddress = devs[i].macAddr;
      x->bitalinoDev = new BITalino(bitalinoaddress.c_str());
#else
      x->bitalinoDev = new BITalino("/dev/tty.bitalino-DevB");
#endif
      post("BITalino version: %s", x->bitalinoDev->version().c_str());
    }
    // BITalino channels : EMG, EDA, ECG, ACCEL, LUX, 5TH_CHANNEL (?)
    BITalino::Vint chans;
    chans.push_back(0);
		chans.push_back(1);
    chans.push_back(2);
    chans.push_back(3);
    chans.push_back(4);
    chans.push_back(5);
    
		// assign digital output states
		BITalino::Vbool outputs;
		outputs.push_back(false);
		outputs.push_back(false);
		outputs.push_back(true);
		outputs.push_back(false);

		x->bitalinoDev->start(1000, chans);
		x->bitalinoDev->trigger(outputs);
    
    bitalino_busy = true;
    x->systhread_cancel = false;
    post("BITalino : connected to device");

    while (1) 
    {
      // test if we're being asked to die, and if so return before we do the work
      if (x->systhread_cancel)
        break;
      
      systhread_mutex_lock(x->mutex);
      
      try
      {
        x->bitalinoDev->read(*(x->frames));
      }
            
      catch (BITalino::Exception &e)
      {
        post("BITalino exception: %s\n", e.getDescription());
        if (e.code == BITalino::Exception::CONTACTING_DEVICE)
        {
          systhread_mutex_unlock(x->mutex);
          bitalino_nopoll(x);
          break;;
        }
      }

      systhread_mutex_unlock(x->mutex);
      qelem_set(x->qelem);				// notify main thread using qelem mechanism
      systhread_sleep(x->sleeptime);
    }
        
    x->bitalinoDev->stop();
    post("BITalino : disconnected from device");
    bitalino_busy = false;
    x->systhread_cancel = false;			// reset cancel flag for next time, in case
    // the thread is created again
    
    systhread_exit(0);						// this can return a value to systhread_join();
    return NULL;
  }
  catch(BITalino::Exception &e)
  {
    post("BITalino exception: %s\n", e.getDescription());

    bitalino_nopoll(x);

		if (x->bitalinoDev != NULL)
		{
			x->bitalinoDev->stop();
      delete x->bitalinoDev;
			x->bitalinoDev = NULL;
		}
		bitalino_busy = false;
		
		if (x->systhread != NULL)
		{
			x->systhread = NULL;
			systhread_exit(0);
		}
		return NULL;
  }
}

void bitalino_qfn(t_bitalino *x)
{
  systhread_mutex_lock(x->mutex);
  for (int i = 0; i < 20; i++) {
    (*(x->local_frames))[i] = (*x->frames)[i];
  }
  systhread_mutex_unlock(x->mutex);
}

void bitalino_clock(t_bitalino *x)
{
  clock_fdelay(x->m_poll, x->poll_interval);
  bitalino_bang(x);
}

void bitalino_bang(t_bitalino *x)
{
  for (int i = 0; i < 20; i++) 
  {
    const BITalino::Frame &f = (*(x->local_frames))[i];
        
    t_atom value_out;
    for(int j = 0; j < 6; j++) 
    {
      atom_setfloat(&value_out, f.analog[j]);
      outlet_anything(x->p_outlet, gensym(x->messages_out[j]), 1, &value_out);
    }
  }
}


void bitalino_start(t_bitalino *x, t_symbol *s, long argc, t_atom *argv)
{
  if(bitalino_busy) 
  {
    post("BITalino : an object instance is already connected");
    return;
  }
    
  if (x->systhread == NULL) 
  {
    //post("starting thread");
    systhread_create((method) bitalino_get, x, 0, 0, 0, &x->systhread);
  }
}

void bitalino_stop(t_bitalino *x)
{
	unsigned int ret;
    
  bitalino_nopoll(x);
	
  if (x->systhread)
  {
    //post("stopping thread");
    x->systhread_cancel = true;			// tell the thread to stop
    systhread_join(x->systhread, &ret);	// wait for the thread to stop
    x->systhread = NULL;
  }
}

void bitalino_disconnect(t_bitalino *x)
{
	bitalino_stop(x);

	if(x->bitalinoDev != NULL)
	{
		delete x->bitalinoDev;
		x->bitalinoDev = NULL;
	}
}

void bitalino_poll(t_bitalino *x, long n)
{
  if(n == 0) {
    bitalino_nopoll(x);
  } else {
    x->poll_interval = static_cast<double>(n);
    clock_fdelay(x->m_poll, 0.);
    //post("start polling BITalino\n");
  }
}

void bitalino_nopoll(t_bitalino *x)
{
  clock_unset(x->m_poll);
  //post("stop polling BITalino\n");
}
