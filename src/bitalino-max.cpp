/**
 *
 * @file bitalino-max.cpp
 * @author joseph.larralde@ircam.fr
 * @author Riccardo.Borghesi@ircam.fr
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
#include "ext_systhread.h"
#ifdef WIN32
#include <sys/select.h>
#endif
#include <stdio.h>
#include <queue>
#include <map>

#define BIT_NFRAMES 20
#define BIT_MAXFRAMES 120
#define BIT_MAXCTLFRAMES 10 // for pwm and digi out
#define BIT_BT_REQUEST_INTERVAL 10
#define BIT_ASYNC_POLL_INTERVAL 20
#define BIT_DEF_SYNC_POLL_INTERVAL 2

// Global vars prevent other objects to interfere with devices currently in use.
// First object to start on a specific port gets the exclusive connection
// until it releases it.
std::map<std::string, bool> busy_bitalinos;

/**
 * @todo add a method to control buffer queues sizes
 */

typedef struct _bitalino {
  t_object p_ob;
  
  t_systhread         systhread;        // thread reference
  t_systhread_mutex   mutex;            // mutual exclusion lock for threadsafety
  t_systhread_mutex   qmutex;           // only used by queue
  int                 systhread_cancel;	// thread cancel flag
  void                *qelem;           // for message passing between threads
  int                 sleeptime;
  
  unsigned char       automatic;
  unsigned char       continuous;
  
  bool                query_state;
  bool                got_state;
  BITalino::State     state;
  
  std::queue<std::vector<bool>> digiout_buffer;
  std::queue<int>               pwmout_buffer;
  int                           bat_threshold;
  
  BITalino::VFrame    *frames;
  //BITalino::VFrame    *local_frames;
  //BITalino::VFrame    *frame_buffer;
  std::queue<BITalino::Frame> *frame_buffer;
  //bool                new_frame;
  //bool                first_frame;
  unsigned char       frame_zero_id;
  
  const char          *analog_messages_out[6];
  const char          *digital_messages_out[4];
  void                *m_poll;
  double              poll_interval;
  void                *p_outlet;
  
  bool                connected;
  int                 bitalino_version;
  int                 bitalino_id;
  std::string         bitalino_mac;
  std::string         bitalino_portname;  // can be "ab-cd" (with numbers) or "anonymous"
} t_bitalino;

void bitalino_find(t_bitalino *x);
void bitalino_getstate(t_bitalino *x);
void bitalino_battery(t_bitalino *x, long n);
void bitalino_pwm(t_bitalino *x, long n);
void bitalino_trigger(t_bitalino *x, t_symbol *s, long argc, t_atom *argv);
//void bitalino_anything(t_bitalino *x, t_symbol *s, long argc, t_atom *argv);

void bitalino_connect(t_bitalino *x, t_symbol *s, long argc, t_atom *argv);
void bitalino_disconnect(t_bitalino *x);

void bitalino_bang(t_bitalino *x);
void *bitalino_get(t_bitalino *x);  // threaded function
void bitalino_qfn(t_bitalino *x);   // function writing frames to thread-safe local frames
void bitalino_clock(t_bitalino *x);
void bitalino_start(t_bitalino *x, t_symbol *s, long argc, t_atom *argv);
void bitalino_stop(t_bitalino *x);
void bitalino_poll(t_bitalino *x);
void bitalino_poll(t_bitalino *x, long n);
void bitalino_nopoll(t_bitalino *x);
void bitalino_assist(t_bitalino *x, void *b, long m, long a, char *s);
void *bitalino_new(t_symbol *s, long argc, t_atom *argv);
void bitalino_free(t_bitalino *x);
//================================ ATTRIBUTE GETTERS / SETTERS :
t_max_err bitalino_get_automatic(t_bitalino *x, t_object *attr,
                                 long *argc, t_atom **argv);
t_max_err bitalino_set_automatic(t_bitalino *x, t_object *attr,
                                 long argc, t_atom *argv);

t_class *bitalino_class;


//--------------------------------------------------------------------------

// main method called only once in a Max session
int C74_EXPORT main(void)
{
  t_class *c;
  
  c = class_new("bitalino", (method)bitalino_new, (method)bitalino_free,
                sizeof(t_bitalino), 0L, A_GIMME, 0);
  
  class_addmethod(c, (method)bitalino_connect,    "connect",    A_GIMME,  0);
  // (optional) assistance method needs to be declared like this
  class_addmethod(c, (method)bitalino_assist,     "assist",     A_CANT,   0);
  class_addmethod(c, (method)bitalino_disconnect, "disconnect",           0);
  // try this with windows later, doesn't work on osx :
  //class_addmethod(c, (method)bitalino_find,       "find",                 0);
  //class_addmethod(c, (method)bitalino_bang,       "bang",                 0);
  class_addmethod(c, (method)bitalino_getstate,   "getstate",             0);
  class_addmethod(c, (method)bitalino_battery,    "battery",    A_LONG,   0);
  class_addmethod(c, (method)bitalino_pwm,        "pwm",        A_LONG,   0);
  class_addmethod(c, (method)bitalino_trigger,    "trigger",    A_GIMME,  0);
  //class_addmethod(c, (method)bitalino_anything,   "anything",   A_GIMME,  0);
  
  CLASS_ATTR_CHAR       (c, "automatic",    0, t_bitalino, automatic);
  CLASS_ATTR_STYLE_LABEL(c, "automatic",    0, "onoff",
                         "automatic frames polling");
  //CLASS_ATTR_DEFAULT    (c, "automatic", 0, "255");
  CLASS_ATTR_ACCESSORS  (c, "automatic", bitalino_get_automatic, bitalino_set_automatic);
  
  CLASS_ATTR_CHAR       (c, "continuous", 0, t_bitalino, continuous);
  CLASS_ATTR_STYLE_LABEL(c, "continuous", 0, "onoff",
                         "continuous output of values (if automatic enabled)");
  //CLASS_ATTR_DEFAULT    (c, "continuous", 0, "255");
  
  CLASS_ATTR_DOUBLE     (c, "interval",   0, t_bitalino, poll_interval);
  
  class_register(CLASS_BOX, c);
  bitalino_class = c;
  
  post("bitalino object loaded");
  return 0;
}


//--------------------------------------------------------------------------

void *bitalino_new(t_symbol *s, long argc, t_atom *argv)
{
  t_bitalino *x;
  
  x = (t_bitalino *)object_alloc(bitalino_class);
  x->analog_messages_out[0] = "/A1";
  x->analog_messages_out[1] = "/A2";
  x->analog_messages_out[2] = "/A3";
  x->analog_messages_out[3] = "/A4";
  x->analog_messages_out[4] = "/A5";
  x->analog_messages_out[5] = "/A6";
  
  x->digital_messages_out[0] = "/I1";
  x->digital_messages_out[1] = "/I2";
  x->digital_messages_out[2] = "/XX"; // depends on BITalino version (set in 'get')
  x->digital_messages_out[3] = "/XX"; // depends on BITalino version
  
  x->p_outlet = outlet_new(x, NULL);
  
  x->qelem = qelem_new(x,(method)bitalino_qfn);
  x->systhread = NULL;
  systhread_mutex_new(&x->mutex,0);
  systhread_mutex_new(&x->qmutex,0);
  
  x->sleeptime = BIT_BT_REQUEST_INTERVAL;
  x->frames = new BITalino::VFrame(BIT_NFRAMES);
  x->poll_interval = BIT_DEF_SYNC_POLL_INTERVAL;
  x->m_poll = clock_new((t_object *)x, (method)bitalino_clock);
  //x->local_frames = new BITalino::VFrame(BIT_NFRAMES);
  
  x->frame_buffer = new std::queue<BITalino::Frame>();//new BITalino::VFrame(100);
  x->frame_zero_id = 0;
  //x->new_frame = false;
  
  x->connected = false;
  x->bitalino_version = 0;
  x->bitalino_id = 0;
  x->bitalino_mac = "";
  x->bitalino_portname = "";
  
  x->automatic = 1;
  x->continuous = 1;
  x->query_state = false;
  x->got_state = false;
  
  x->bat_threshold = -1;
  
  attr_args_process(x, argc, argv);
  
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
  delete(x->frame_buffer);
}

//------------------------------------------------------------------------------

void bitalino_assist(t_bitalino *x, void *b, long m, long a, char *s)
{
  if (m == ASSIST_OUTLET) {
    sprintf(s,"OSC-style BITalino channels messages");
  } else {
    switch (a) {
      case 0:
        sprintf(s,"connect [mac-suffix], disconnect, getstate, battery [0;63], \
                   pwm [0;255], trigger <0/1 0/1 [0/1 0/1]>");
        break;
    }
  }
}

//------------------------------------------------------------------------------

void bitalino_getstate(t_bitalino *x) {
  if (!x->connected) {
    post("no BITalino connected");
    return;
  }
  
  if(x->bitalino_version < 2) {
    post("sorry, BITalino v1 doesn't support the state command");
  } else {
    x->query_state = true;
  }
}

void bitalino_battery(t_bitalino *x, long n) {
  if (!x->connected) {
    post("no BITalino connected");
    return;
  }
  
  int val = n > 63 ? 63 : (n < 0 ? 0 : n);
  x->bat_threshold = val;
}

void bitalino_pwm(t_bitalino *x, long n) {
  if (!x->connected) {
    post("no BITalino connected");
    return;
  }
  
  if(x->bitalino_version < 2) {
    post("sorry, BITalino v1 doesn't support the pwm command");
    return;
  } else {
    int val = n > 255 ? 255 : (n < 0 ? 0 : n);
    x->pwmout_buffer.push(val);
    if (x->pwmout_buffer.size() > BIT_MAXCTLFRAMES) {
      x->pwmout_buffer.front();
      x->pwmout_buffer.pop();
    }
  }
}

void bitalino_trigger(t_bitalino *x, t_symbol *s, long argc, t_atom *argv) {
  if (!x->connected) {
    post("no BITalino connected");
    return;
  }
  
  BITalino::Vbool dig;
  for (int i = 0; i < 2; i++) {
    dig.push_back(false);
  }
  if (x->bitalino_version < 2) {
    for (int i = 0; i < 2; i++) {
      dig.push_back(false);
    }
  }
  int tot = fmin(argc, 4);
  for (int i = 0; i < tot; i++) {
    dig[i] = atom_getlong(argv + i) > 0;
  }
  x->digiout_buffer.push(dig);
  if (x->digiout_buffer.size() > BIT_MAXCTLFRAMES) {
    x->digiout_buffer.front();
    x->digiout_buffer.pop();
  }
}

//------------------------------------------------------------------------------

void *bitalino_get(t_bitalino *x)
{
  try {
        
#ifdef WIN32
    
    std::string bitalinoaddress = "COM5";
    
    post("BITalino: looking for device");
    
    BITalino::VDevInfo devs = BITalino::find();
    for (int i = 0; i < devs.size(); i++)
      if (_memicmp(devs[i].name.c_str(), "bitalino", 8) == 0)
        bitalinoaddress = devs[i].macAddr;
    BITalino dev(bitalinoaddress.c_str());
    
#else

    if (x->bitalino_portname == "unknown") {
      x->bitalino_portname = "/dev/tty.BITalino-DevB";
      x->bitalino_version = 2;

      try {
        BITalino dev(x->bitalino_portname.c_str());
      } catch (BITalino::Exception &e) {
        x->bitalino_portname = "/dev/tty.bitalino-DevB";
        x->bitalino_version = 1;
      }
    }
    
    BITalino dev(x->bitalino_portname.c_str());
    x->connected = true;

#endif //WIN32

    //BITalino dev("/dev/tty.bitalino-DevB");
    
    post("BITalino version: %s", dev.version().c_str());
    
    // BITalino channels :
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
    
    if (x->bitalino_version < 2) {
      outputs.push_back(false);
      outputs.push_back(false);
      x->digital_messages_out[2] = "/I3";
      x->digital_messages_out[3] = "/I4";
    } else {
      x->digital_messages_out[2] = "/O1";
      x->digital_messages_out[3] = "/O2";
    }
    
    dev.start(1000, chans);
    dev.trigger(outputs);
    
    busy_bitalinos[x->bitalino_portname] = true;
    x->systhread_cancel = false;
    post("BITalino : connected to device");
    
    while (1) {
        
      // test if we're being asked to die, and if so return before we do the work
      if (x->systhread_cancel)
        break;
      
      // if we're not asked to spit a stream of values,
      // don't leave the loop so bitalino connection is kept alive
      
      systhread_mutex_lock(x->mutex);
      
      // these calls need the device not to be in acquisition :
      
      if (x->query_state || x->bat_threshold >= 0) {
        dev.stop();
        if (x->query_state) {
          try {
            x->state = dev.state();
            x->query_state = false;
            x->got_state = true;
          } catch (BITalino::Exception &e) {
            post("BITalino exception %s\n", e.getDescription());
            
            if (e.code == BITalino::Exception::INVALID_PARAMETER) {
              post("problem in call to state");
            }
          }
        }
        if (x->bat_threshold >= 0) {
          try {
            dev.battery(x->bat_threshold);
            x->bat_threshold = -1;
          } catch (BITalino::Exception &e) {
            post("BITalino exception %s\n", e.getDescription());

            if (e.code == BITalino::Exception::INVALID_PARAMETER) {
              post("invalid parameter for battery");
            }
          }
        }
        dev.start(1000, chans);
      }
      
      // replaced "while" by "if" to avoid freezing when buffer is full
      // (too many messages in), for pwmout and digiout
      
      if (x->pwmout_buffer.size() > 0) {
        try {
          dev.pwm(x->pwmout_buffer.front());
          x->pwmout_buffer.pop();
        } catch (BITalino::Exception &e) {
          post("BITalino exception %s\n", e.getDescription());
          
          if (e.code == BITalino::Exception::INVALID_PARAMETER) {
            post("invalid parameter for pwm\n");
          }
        }
      }
      
      if (x->digiout_buffer.size() > 0) {
        try {
          dev.trigger(x->digiout_buffer.front());
          x->digiout_buffer.pop();
        } catch (BITalino::Exception &e) {
          post("BITalino exception %s\n", e.getDescription());
          
          if (e.code == BITalino::Exception::INVALID_PARAMETER) {
            post("invalid parameter for trigger");
          }
        }
      }
      
      if (!x->automatic) {
        systhread_mutex_unlock(x->mutex);
        qelem_set(x->qelem);	// notify main thread using qelem mechanism
        systhread_sleep(x->sleeptime);
        continue;
      }
      
      //============== if @automatic is on, continue ==============//
      
      try {
        dev.read(*(x->frames));
      } catch (BITalino::Exception &e) {
        post("BITalino exception: %s\n", e.getDescription());
          
        if (e.code == BITalino::Exception::CONTACTING_DEVICE) {
          systhread_mutex_unlock(x->mutex);
          bitalino_nopoll(x);
          break;
        }
      }
      
      systhread_mutex_unlock(x->mutex);
      qelem_set(x->qelem);	// notify main thread using qelem mechanism
      systhread_sleep(x->sleeptime);
    }
    
    dev.stop();
    post("BITalino : disconnected from device");
    x->connected = false;
    busy_bitalinos[x->bitalino_portname] = false;
    // reset cancel flag for next time, in case the thread is created again
    x->systhread_cancel = false;
    systhread_exit(0);
    // this can return a value to systhread_join();
    return NULL;
    
  } catch (BITalino::Exception &e) {
    post("BITalino exception: %s\n", e.getDescription());
    busy_bitalinos[x->bitalino_portname] = false;
    bitalino_stop(x);
    // this can return a value to systhread_join();
    return NULL;
  }
}

void bitalino_qfn(t_bitalino *x)
{
  systhread_mutex_lock(x->mutex);
    
  if (x->frame_zero_id != (*x->frames)[0].seq) {
    x->frame_zero_id = (*x->frames)[0].seq;
        
    systhread_mutex_lock(x->qmutex);
    for (int i=0; i<BIT_NFRAMES; i++) {
      x->frame_buffer->push((*x->frames)[i]);
    }

    // CONTINUOUS MODE
    if (x->continuous) {
      while (x->frame_buffer->size() > BIT_MAXFRAMES) {
        x->frame_buffer->front();
        x->frame_buffer->pop();
      }
    }
    
    systhread_mutex_unlock(x->qmutex);
  }
    
  systhread_mutex_unlock(x->mutex);
}

void bitalino_clock(t_bitalino *x)
{
  if (x->continuous) {
    clock_fdelay(x->m_poll, x->poll_interval);
  } else {
    clock_fdelay(x->m_poll, static_cast<double>(BIT_ASYNC_POLL_INTERVAL));
  }
  bitalino_bang(x);
}

void bitalino_bang(t_bitalino *x)
{
  if (x->got_state) {
    const BITalino::State s = x->state;
    t_atom value_out;
    std::string msgOutStr;

    for (int i = 0; i < 6; i++) {
      atom_setlong(&value_out, s.analog[i]);
      msgOutStr = "/state" + std::string(x->analog_messages_out[i]);
      outlet_anything(x->p_outlet, gensym(msgOutStr.c_str()), 1, &value_out);
    }

    atom_setlong(&value_out, s.battery);
    outlet_anything(x->p_outlet, gensym("/state/battery"), 1, &value_out);

    atom_setlong(&value_out, s.batThreshold);
    outlet_anything(x->p_outlet, gensym("/state/battery_threshold"), 1, &value_out);
    
    for (int i = 0; i < 4; i++) {
      atom_setlong(&value_out, s.digital[i] ? 1 : 0);
      msgOutStr = "/state" + std::string(x->analog_messages_out[i]);
      outlet_anything(x->p_outlet, gensym(msgOutStr.c_str()), 1, &value_out);
    }
    
    x->got_state = false;
  }
  
  if (!x->automatic) return;
  
  // CONTINUOUS MODE
  if (x->continuous) {
    if (!x->frame_buffer->empty()) {
      const BITalino::Frame &f = x->frame_buffer->front();
      t_atom value_out;
      for (int j = 0; j < 6; j++) {
        atom_setfloat(&value_out, f.analog[j]);
        outlet_anything(x->p_outlet, gensym(x->analog_messages_out[j]),
                        1, &value_out);
      }
      for (int j = 0; j < 4; j++) {
        atom_setfloat(&value_out, f.digital[j]);
        outlet_anything(x->p_outlet, gensym(x->digital_messages_out[j]),
                        1, &value_out);
      }
      
      systhread_mutex_lock(x->qmutex);
      
      if (x->frame_buffer->size() > 1 && !x->frame_buffer->empty()) {
        x->frame_buffer->pop();
      }
      systhread_mutex_unlock(x->qmutex);
    }
    
  } else {
    systhread_mutex_lock(x->qmutex);
    while (!x->frame_buffer->empty()) {
      const BITalino::Frame &f = x->frame_buffer->front();
      t_atom value_out;
      for (int j = 0; j < 6; j++) {
        atom_setfloat(&value_out, f.analog[j]);
        outlet_anything(x->p_outlet, gensym(x->analog_messages_out[j]),
                        1, &value_out);
      }
      for (int j = 0; j < 4; j++) {
        atom_setfloat(&value_out, f.digital[j]);
        outlet_anything(x->p_outlet, gensym(x->digital_messages_out[j]),
                        1, &value_out);
      }
      x->frame_buffer->pop();
    }
    systhread_mutex_unlock(x->qmutex);
  }
}

// this doesn't seem to work (at least on osx) :
void bitalino_find(t_bitalino *x) {
  try {
    BITalino::VDevInfo info = BITalino::find();
    post("list of found BITalino devices :\n");
    for (int i = 0; i < info.size(); i++) {
      std::string line = "mac : " + info[i].macAddr +
                         ", name : " + info[i].name;
      post(line.c_str());
    }
  } catch (BITalino::Exception &e) {
    post("BITalino exception : %s\n", e.getDescription());
  }
}

void bitalino_connect(t_bitalino *x, t_symbol *s, long argc, t_atom *argv)
{
  bitalino_start(x, s, argc, argv);
  bitalino_poll(x);
}

void bitalino_start(t_bitalino *x, t_symbol *s, long argc, t_atom *argv)
{
  x->bitalino_version = 0;
  x->bitalino_id = 0;
  x->bitalino_mac = "";
  x->bitalino_portname = "unknown";
  
  if (argc > 0) {
    std::string arg1 = std::string(atom_getsym(argv)->s_name);
    if (arg1 == "v1") {
      x->bitalino_version = 1;
      if (argc > 1) {
        int arg2 = atom_getlong(argv + 1);
        x->bitalino_id = arg2;
        x->bitalino_portname = "/dev/tty.bitalino-DevB-" + std::to_string(arg2);
      } else {
        x->bitalino_portname = "/dev/tty.bitalino-DevB";
      }
    } else if (arg1 == "v2") {
      x->bitalino_version = 2;
      if (argc > 1) {
        switch (atom_gettype(argv + 1)) {
          case A_LONG:
          {
            int longarg2 = atom_getlong(argv + 1);
            x->bitalino_id = longarg2;
            x->bitalino_portname = "/dev/tty.BITalino-DevB-" +
                                   std::to_string(longarg2);
          }
          break;
            
          case A_SYM:
          {
            std::string strarg2 = std::string(atom_getsym(argv + 1)->s_name);
            x->bitalino_mac = strarg2;
            x->bitalino_portname = "/dev/tty.BITalino-" + strarg2 + "-DevB";
          }
          break;
            
          default:
          {
            x->bitalino_portname = "/dev/tty.BITalino-DevB";
          }
          break;
        }
      } else {
        x->bitalino_portname = "/dev/tty.BITalino-DevB";
      }
    } else {
      x->bitalino_version = 2;
      x->bitalino_mac = std::string(atom_getsym(argv)->s_name);
      x->bitalino_portname = "/dev/tty.BITalino-" + x->bitalino_mac + "-DevB";
    }
  } else {
    x->bitalino_portname = "unknown";
  }
  
  
  if (busy_bitalinos.find(x->bitalino_portname) == busy_bitalinos.end()) {
    busy_bitalinos[x->bitalino_portname] = false;
  } else {
    if (busy_bitalinos[x->bitalino_portname]) {
      post("BITalino : port already used");
      return;
    } else {
      // do nothing
    }
  }
  
  if (x->systhread == NULL) {
    //post("starting thread");
    systhread_create((method) bitalino_get, x, 0, 0, 0, &x->systhread);
  }
}

void bitalino_disconnect(t_bitalino *x)
{
  bitalino_stop(x);
}

void bitalino_stop(t_bitalino *x)
{
  unsigned int ret;
    
  bitalino_nopoll(x);
    
  if (x->systhread) {
    //post("stopping thread");
    x->systhread_cancel = true;			// tell the thread to stop
    systhread_join(x->systhread, &ret);	// wait for the thread to stop
    //busy_bitalinos[x->bitalino_id] = false;
    x->systhread = NULL;
  }
}


void bitalino_poll(t_bitalino *x, long n)
{
  if (n == 0) {
    bitalino_nopoll(x);
  } else {
    x->poll_interval = static_cast<double>(n);
    clock_fdelay(x->m_poll, 0.);
    //post("start polling BITalino\n");
  }
}

void bitalino_poll(t_bitalino *x)
{
  clock_fdelay(x->m_poll, 0.);
}

void bitalino_nopoll(t_bitalino *x)
{
  clock_unset(x->m_poll);
  //post("stop polling BITalino\n");
}

//======================= attribute getters / setters ========================//

t_max_err bitalino_get_automatic(t_bitalino *x, t_object *attr,
                                 long *argc, t_atom **argv)
{
  if (argc && argv) {
    char alloc;
    if (atom_alloc(argc, argv, &alloc)) {
      return MAX_ERR_GENERIC;
    }
    atom_setchar_array(*argc, *argv, 1, &x->automatic);
    //post("automatic getter called");
  }
  return MAX_ERR_NONE;
}

t_max_err bitalino_set_automatic(t_bitalino *x, t_object *attr,
                                 long argc, t_atom *argv)
{
  if(argc && argv) {
    unsigned char prev = x->automatic;
    atom_getchar_array(argc, argv, 1, &x->automatic);
    if (x->automatic != prev) {
      if (x->automatic == 0) {
//        bitalino_poll(x);
//        post("polling enabled");
      } else {
//        bitalino_nopoll(x);
//        post("polling disabled");
      }
    }
    //post("automatic setter called  with value %ld", x->automatic);
  }
  return MAX_ERR_NONE;
}


