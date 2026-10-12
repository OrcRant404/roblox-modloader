// Standalone adapter test; mocks engine CAPI and native host, without launching Studio.
#include <cassert>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <iostream>
#include <RobloxModLoader/luau/engine_api.hpp>
#include <RobloxModLoader/luau/native_api.hpp>
struct Value;
struct Object {
  std::map<std::string,Value> fields;
  std::map<int,Value> array;
  bool frozen {};
  bool bad_index {};
};
struct Function {
  EngineCFunction native {};
  std::function<int(lua_State*)> body;
  std::vector<Value> upvalues;
};
struct Value {
  int tag {};
  std::string text;
  double number {};
  bool boolean {};
  std::shared_ptr<Object> object;
  std::shared_ptr<Function> function;
};
struct Frame {
  std::size_t base {};
  std::shared_ptr<Function> function;
};
struct lua_State {
  std::vector<Value> stack;
  std::vector<Frame> frames;
  Value globals;
  Value registry;
  std::deque<Value> tasks;
};
struct LuaFailure:std::runtime_error {
  using std::runtime_error::runtime_error;
};
static Value nil;
static Value str(std::string s) {
  Value v;
  v.tag=6;
  v.text=std::move(s);
  return v;
}

static Value num(double n) {
  Value v;
  v.tag=3;
  v.number=n;
  return v;
}

static Value boolean(bool b) {
  Value v;
  v.tag=1;
  v.boolean=b;
  return v;
}

static Value object(int tag=7) {
  Value v;
  v.tag=tag;
  v.object=std::make_shared<Object>();
  return v;
}

static Value fn(std::function<int(lua_State*)> body) {
  Value v;
  v.tag=8;
  v.function=std::make_shared<Function>();
  v.function->body=std::move(body);
  return v;
}

static std::size_t base(lua_State* s) {
  return s->frames.empty()?0:s->frames.back().base;
}

static Value& value(lua_State* s,int idx) {
  if(idx==-10000)return s->registry;
  if(idx==-10002)return s->globals;
  if(idx<=-10003) {
    int up=-10002-idx-1;
    if(!s->frames.empty()&&up>=0&&static_cast<std::size_t>(up)<s->frames.back().function->upvalues.size())return s->frames.back().function->upvalues[up];
    return nil;
  }

const long pos=idx>0?static_cast<long>(base(s))+idx-1:static_cast<long>(s->stack.size())+idx;
  return pos>=static_cast<long>(base(s))&&pos<static_cast<long>(s->stack.size())?s->stack[pos]:nil;
}

static int gettop(lua_State*s) {
  return static_cast<int>(s->stack.size()-base(s));
}

static void settop(lua_State*s,int n) {
  s->stack.resize(n>=0?base(s)+n:s->stack.size()+n+1);
}

static int type(lua_State*s,int i) {
  return value(s,i).tag;
}

static void pushvalue(lua_State*s,int i) {
  Value v=value(s,i);
  s->stack.push_back(std::move(v));
}

static void pushnil(lua_State*s) {
  s->stack.emplace_back();
}

static void pushstring(lua_State*s,const char*t,std::size_t n) {
  s->stack.push_back(str(std::string(t,n)));
}

static void pushnumber(lua_State*s,double n) {
  s->stack.push_back(num(n));
}

static void pushboolean(lua_State*s,int n) {
  s->stack.push_back(boolean(n!=0));
}

static const char* tostring(lua_State*s,int i,std::size_t*n) {
  auto&v=value(s,i);
  if(v.tag!=6)return nullptr;
  if(n)*n=v.text.size();
  return v.text.c_str();
}

static double tonumber(lua_State*s,int i,int*n) {
  auto&v=value(s,i);
  if(n)*n=v.tag==3;
  return v.tag==3?v.number:0;
}

static int toboolean(lua_State*s,int i) {
  auto&v=value(s,i);
  return v.tag&&!(v.tag==1&&!v.boolean);
}

static void createtable(lua_State*s,int,int) {
  s->stack.push_back(object());
}

static int getfield(lua_State*s,int i,const char*k) {
  Value v=value(s,i);
  if(!v.object)throw LuaFailure("not an object");
  if(v.object->bad_index)throw LuaFailure("fake __index error");
  s->stack.push_back(v.object->fields[k]);
  return s->stack.back().tag;
}

static void setfield(lua_State*s,int i,const char*k) {
  Value v=value(s,i);
  if(!v.object||v.object->frozen)throw LuaFailure("readonly field");
  v.object->fields[k]=s->stack.back();
  s->stack.pop_back();
}

static void readonly(lua_State*s,int i,int n) {
  value(s,i).object->frozen=n;
}

static int getreadonly(lua_State*s,int i) {
  return value(s,i).object->frozen;
}

static int rawgeti(lua_State*s,int i,int n) {
  Value v=value(s,i);
  if(!v.object)throw LuaFailure("raw index is not a table");
  auto found=v.object->array.find(n);
  s->stack.push_back(found==v.object->array.end()?Value {}:found->second);
  return s->stack.back().tag;
}

static void rawseti(lua_State*s,int i,int n) {
  Value v=value(s,i);
  if(!v.object||v.object->frozen)throw LuaFailure("readonly array");
  if(s->stack.back().tag)v.object->array[n]=s->stack.back();
  else v.object->array.erase(n);
  s->stack.pop_back();
}

static int rawequal(lua_State*s,int a,int b) {
  auto&x=value(s,a);
  auto&y=value(s,b);
  return x.tag==y.tag&&(x.object&&x.object==y.object);
}

static void closure(lua_State*s,EngineCFunction c,const char*,int n,EngineContinuation) {
  Value v=fn( {});
  v.function->native=c;
  v.function->upvalues.assign(s->stack.end()-n,s->stack.end());
  s->stack.resize(s->stack.size()-n);
  s->stack.push_back(v);
}

static int pcall(lua_State*s,int nargs,int nresults,int) {
  const auto pos=s->stack.size()-nargs-1;
  Value f=s->stack[pos];
  s->frames.push_back( {
    pos+1,f.function
  });
  try {
    if(f.tag!=8||!f.function)throw LuaFailure("not callable");
    int n=f.function->native?f.function->native(s):f.function->body(s);
    std::vector<Value> results(s->stack.end()-n,s->stack.end());
    s->stack.resize(pos);
    s->frames.pop_back();
    if(nresults>=0)results.resize(nresults);
    s->stack.insert(s->stack.end(),results.begin(),results.end());
    return 0;
  } catch(const LuaFailure&e) {
    s->stack.resize(pos);
    s->frames.pop_back();
    s->stack.push_back(str(e.what()));
    return 2;
  }
}

static EngineApi api;
static Value call(lua_State*s,Value f,std::vector<Value> args= {}) {
  const auto before=s->stack.size();
  s->stack.push_back(f);
  s->stack.insert(s->stack.end(),args.begin(),args.end());
  const int status=pcall(s,args.size(),1,0);
  assert(status==0);
  Value out=s->stack.back();
  s->stack.resize(before);
  return out;
} struct Captured {
  RmlNativeApiEngineCallback callback;
  void*data;
};
static std::map<std::string,Captured> registered;
extern "C" RmlNativeApiStatus rml_native_api_register_engine_export(RmlNativeApiOwner*,const char*n,RmlNativeApiEngineCallback cb,void*d,RmlNativeApiExportSlot*out) {
  registered[n]= {
    cb,d
  };
  *out=registered.size();
  return RML_NATIVE_API_OK;
}

extern "C" RmlNativeApiStatus rml_native_api_owned_string_assign(RmlNativeApiOwnedString*out,const char*s,std::size_t n) {
  out->data=static_cast<char*>(std::malloc(n+1));
  std::memcpy(out->data,s,n);
  out->data[n]=0;
  out->size=n;
  return RML_NATIVE_API_OK;
}

extern "C" void rml_native_api_owned_string_destroy(RmlNativeApiOwnedString*out) {
  std::free(out->data);
  *out= {};
}

static std::vector<Value> exported(lua_State*s,const std::string&n,std::vector<Value>args,bool fails=false) {
  const auto before=s->stack.size();
  const auto oldframes=s->frames.size();
  s->frames.push_back( {
    before, {}
  });
  s->stack.insert(s->stack.end(),args.begin(),args.end());
  int count {};
  RmlNativeApiOwnedString error {};
  RmlNativeApiEngineContext ctx {
    1,s,&api
  };
  const auto captured=registered.at(n);
  auto status=captured.callback(captured.data,&ctx,&count,&error);
  if(fails) {
    assert(status==RML_NATIVE_API_CALLBACK_FAILURE);
    assert(count==0);
    assert(error.data);
    rml_native_api_owned_string_destroy(&error);
    s->stack.resize(before);
    s->frames.resize(oldframes);
    return {};
  } if(status!=RML_NATIVE_API_CALLBACK_SUCCESS) {
    std::cerr<<n<<": "<<(error.data?error.data:"no error")<<"\n";
    std::abort();
  }

std::vector<Value>out(s->stack.end()-count,s->stack.end());
  s->stack.resize(before);
  s->frames.resize(oldframes);
  return out;
}

#include "../native-workspace/workspace_lua.cpp"

namespace rml::workspace {
  static std::map<std::string,ViewRegistration> views;
  static std::map<std::string,std::string>states;
  static std::vector<std::string>stack;
  static std::vector<Delivery>events;
  static std::deque<std::function<void()>> pending;
  static std::uint64_t serial {},acks {};
  static bool barrier {};
  void start_host(const std::filesystem::path&) {}void stop_host() {}bool host_active() {
    return true;
  }

PanelRegistration associate_panel(OwnerId o,std::string suffix) {
    return {
      "panel:"+suffix,suffix,o
    };
  } std::string builtin_panel(std::string_view n) {
    return "builtin:"+std::string(n);
  }

ViewRegistration register_view(OwnerId o,std::string n,std::shared_ptr<const SplitNode>) {
    ViewRegistration v {
      "view:"+n,n,o
    };
    views[v.id]=v;
    states[v.id]="inactive";
    return v;
  }

ViewRegistration get_view(std::string_view id) {
    const std::string key(id);
    if(views.count(key))return views.at(key);
    return views.at("view:"+key);
  }

PublicViewState get_state(std::string_view id) {
    const auto v=get_view(id);
    return {
      v,states[v.id], {},"applied", {},false
    };
  }

std::vector<ViewRegistration>get_stack() {
    std::vector<ViewRegistration>out;
    for(auto&id:stack)out.push_back(views.at(id));
    return out;
  }

static void emit(std::string id,EventKind kind,EventReason reason,std::string req) {
    events.push_back( {
      views.at(id).owner, {
        kind,id,reason
      },req,++serial
    });
    barrier=true;
  }

RequestInfo enter_view(std::string_view name) {
    auto id=get_view(name).id;
    auto request="request:"+std::to_string(++serial);
    auto action=[id,request] {
      if(!stack.empty()) {
        states[stack.back()]="suspended";
        emit(stack.back(),EventKind::Exited,EventReason::Covered,request);
      }stack.push_back(id);
      states[id]="active";
      emit(id,EventKind::Entered,EventReason::Enter,request);
    };
    if(barrier)pending.push_back(action);
    else action();
    return {
      request,"queued", {}, {}
    };
  }

RequestInfo exit_view(std::string_view name) {
    auto id=get_view(name).id;
    emit(id,EventKind::Exited,EventReason::Exit,"exit");
    states[id]="inactive";
    std::erase(stack,id);
    return {
      "exit","queued", {}, {}
    };
  }

RequestInfo unregister_view(std::string_view name) {
    auto id=get_view(name).id;
    auto req=exit_view(id);
    states[id]="unregistered";
    events.back().event.reason=EventReason::Unregister;
    return req;
  }

RequestInfo open_panel(std::string_view) {
    return {
      "open","queued", {}, {}
    };
  }RequestInfo close_panel(std::string_view) {
    return {
      "close","queued", {}, {}
    };
  }

RequestInfo inspect_panels() {
    return {
      "inspect","queued", {}, {}
    };
  }RequestInfo recover_layout() {
    return {
      "recover","queued", {}, {}
    };
  }RequestInfo get_request(std::string_view id) {
    return {
      std::string(id),"applied", {},"report"
    };
  }

std::vector<PanelInfo> latest_panels() {
    return {
      {
        "native:Viewport",true,false,true,1,2,300,200,20,20
      }
    };
  }

std::vector<Delivery>take_events(const std::vector<OwnerId>& owners) {
    std::vector<Delivery>out;
    for(auto it=events.begin();it!=events.end();) {
      if(std::find(owners.begin(),owners.end(),it->owner)!=owners.end()) {
        out.push_back(*it);
        it=events.erase(it);
      }else ++it;
    }return out;
  }

void complete_events(const std::vector<Delivery>&e) {
    if(e.empty())return;
    ++acks;
    barrier=!events.empty();
    if(!barrier&&!pending.empty()) {
      auto f=pending.front();
      pending.pop_front();
      f();
    }
  }

void release_owner(OwnerId o) {
    for(auto&[id,v]:views) {
      if(v.owner==o) {
        states[id]="unregistered";
        std::erase(stack,id);
      }
    }
    std::erase_if(events,[o](auto&e) {
      return e.owner==o;
    });
  }

const char* to_string(EventKind e) noexcept {
    return e==EventKind::Entered?"entered":"exited";
  }

const char* to_string(EventReason e) noexcept {
    switch(e) {
      case EventReason::Enter:return "enter";
      case EventReason::Resume:return "resume";
      case EventReason::Covered:return "covered";
      case EventReason::Exit:return "exit";
      case EventReason::Unregister:return "unregister";
    }return "unknown";
  }
}

static std::vector<Value> signal_callbacks;
static Value signal() {
  Value s=object(9);
  s.object->fields["Connect"]=fn([](lua_State*l) {
    Value callback=value(l,2);
    signal_callbacks.push_back(callback);
    Value c=object(9);
    c.object->fields["Disconnect"]=fn([](lua_State*) {
      return 0;
    });
    l->stack.push_back(c);
    return 1;
  });
  return s;
}

static int constructor_calls;
static Value widget(std::string class_name="DockWidgetPluginGui") {
  Value w=object(9);
  w.object->fields["IsA"]=fn([class_name=std::move(class_name)](lua_State*l) {
    assert(gettop(l)==2);
    pushboolean(l,value(l,2).text==class_name);
    return 1;
  });
  w.object->fields["Enabled"]=boolean(true);
  w.object->fields["Destroy"]=fn([](lua_State*) {
    return 0;
  });
  return w;
}

static Value plugin() {
  Value p=object(9);
  p.object->fields["Unloading"]=signal();
  const auto forbidden=fn([](lua_State*)->int {
    ++constructor_calls;
    throw LuaFailure("a native export invoked a yielding widget constructor");
  });
  p.object->fields["CreateDockWidgetPluginGui"]=forbidden;
  p.object->fields["CreateDockWidgetPluginGuiAsync"]=forbidden;
  return p;
}

static Value split(Value first,Value second,bool udim) {
  Value v=object();
  v.object->fields["Direction"]=str("Horizontal");
  Value size=object(udim?9:7);
  size.object->fields["Scale"]=num(.5);
  size.object->fields["Offset"]=num(0);
  v.object->fields["Size"]=size;
  v.object->fields["First"]=first;
  v.object->fields["Second"]=second;
  return v;
}

static Value connect(lua_State*l,Value view,const char*kind,Value callback) {
  Value sig=view.object->fields[kind];
  return call(l,sig.object->fields["Connect"], {
    sig,callback
  });
}

static void drive(lua_State*l,int n) {
  for(int i=0;i<n&&!l->tasks.empty();++i) {
    Value f=l->tasks.front();
    l->tasks.pop_front();
    call(l,f);
  }
}

static Value reference_store(lua_State*l) {
  if(!l->registry.object)return {};
  auto found=l->registry.object->fields.find("__rml_workspace_refs_v1");
  return found==l->registry.object->fields.end()?Value {}:found->second;
}

static std::size_t rooted_count(lua_State*l) {
  const auto root=reference_store(l);
  return root.object?root.object->array.size():0;
}

static int largest_reference(lua_State*l) {
  const auto root=reference_store(l);
  return root.object&&!root.object->array.empty()?root.object->array.rbegin()->first:0;
}

int main() {
  api.checkstack=[](lua_State*,int) {
    return 1;
  };
  api.gettop=&gettop;
  api.settop=&settop;
  api.createtable=&createtable;
  api.pushcclosurek=&closure;
  api.pushlstring=&pushstring;
  api.tolstring=&tostring;
  api.getfield=&getfield;
  api.setfield=&setfield;
  api.setreadonly=&readonly;
  api.getreadonly=&getreadonly;
  api.pushnil=&pushnil;
  api.type=&type;
  api.pushvalue=&pushvalue;
  api.pushboolean=&pushboolean;
  api.pushnumber=&pushnumber;
  api.tonumberx=&tonumber;
  api.toboolean=&toboolean;
  api.rawgeti=&rawgeti;
  api.rawseti=&rawseti;
  api.ref=nullptr;
  api.unref=nullptr;
  api.getref=nullptr;
  api.pcall=&pcall;
  api.mainthread=[](lua_State*l) {
    return l;
  };
  api.rawequal=&rawequal;
  lua_State l;
  l.globals=object();
  l.registry=object();
  Value task=object();
  task.object->fields["delay"]=fn([](lua_State*s) {
    assert(value(s,1).number==.05);
    s->tasks.push_back(value(s,2));
    s->stack.push_back(object(10));
    return 1;
  });
  l.globals.object->fields["task"]=task;
  Value game=object(9);
  game.object->fields["GetService"]=fn([](lua_State*s) {
    assert(value(s,2).text=="RunService");
    Value service=object(9);
    service.object->fields["Heartbeat"]=signal();
    s->stack.push_back(service);
    return 1;
  });
  l.globals.object->fields["game"]=game;
  rml::workspace::register_lua_exports(reinterpret_cast<RmlNativeApiOwner*>(1));
  assert(registered.size()==15);
  Value p=plugin(),p2=plugin(),info=object(9),w=widget();
  exported(&l,"CreatePanel", {
    p,str("Graph"),info
  },true);
  assert(constructor_calls==0&&rooted_count(&l)==0&&l.tasks.empty()&&signal_callbacks.empty());
  exported(&l,"RegisterPanel", {
    p,str("Graph"),object()
  },true);
  exported(&l,"RegisterPanel", {
    p,str("Graph"),widget("Frame")
  },true);
  assert(rooted_count(&l)==0&&l.tasks.empty());
  // All unavailable, readonly, occupied and foreign public registry cases fail
  // through the callback error result without engine private reference calls.
  l.registry= {};
  exported(&l,"RegisterPanel", {
    p,str("Graph"),w
  },true);
  assert(l.registry.tag==0&&rooted_count(&l)==0);
  l.registry=object();
  l.registry.object->frozen=true;
  exported(&l,"RegisterPanel", {
    p,str("Graph"),w
  },true);
  assert(reference_store(&l).tag==0&&rooted_count(&l)==0);
  l.registry=object();
  l.registry.object->fields["__rml_workspace_refs_v1"]=str("foreign");
  exported(&l,"RegisterPanel", {
    p,str("Graph"),w
  },true);
  assert(reference_store(&l).text=="foreign");
  l.registry=object();
  Value foreign_store=object();
  foreign_store.object->fields["__owner"]=str("wrong provider");
  l.registry.object->fields["__rml_workspace_refs_v1"]=foreign_store;
  exported(&l,"RegisterPanel", {
    p,str("Graph"),w
  },true);
  assert(reference_store(&l).object==foreign_store.object&&rooted_count(&l)==0);
  l.registry=object();
  std::vector<Value> created {
    w,exported(&l,"RegisterPanel", {
      p,str("Graph"),w
    })[0]
  };
  assert(created[0].tag==9&&created[1].tag==6);
  assert(created[0].object->fields["Enabled"].boolean);
  assert(reference_store(&l).tag==7&&reference_store(&l).object->fields["__owner"].text.size()==32&&rooted_count(&l)==4);
  const auto first_refs=rooted_count(&l);
  reference_store(&l).object->frozen=true;
  exported(&l,"RegisterPanel", {
    p,str("Other"),widget()
  },true);
  assert(rooted_count(&l)==first_refs);
  reference_store(&l).object->frozen=false;
  auto viewport=exported(&l,"GetBuiltinPanel", {
    str("Viewport")
  })[0];
  auto a=exported(&l,"RegisterView", {
    p,str("A"),split(created[1],viewport,true)
  })[0];
  auto b=exported(&l,"RegisterView", {
    p2,str("B"),split(created[1],viewport,false)
  })[0];
  assert(a.object->frozen&&a.object->fields["Entered"].object->frozen);
  exported(&l,"RegisterPanel", {
    p,str("Graph"),widget()
  },true);
  const auto registration_refs=rooted_count(&l);
  exported(&l,"RegisterPanel", {
    p,str("Other"),w
  },true);
  exported(&l,"RegisterPanel", {
    p2,str("Other"),w
  },true);
  assert(rooted_count(&l)==registration_refs);
  auto bad=split(viewport,viewport,false);
  bad.object->fields["Direction"]=str("Diagonal");
  exported(&l,"RegisterView", {
    p,str("Bad"),bad
  },true);
  auto integer_size=split(viewport,viewport,false);
  integer_size.object->fields["Size"].object->fields["Offset"]=num(0);
  integer_size.object->fields["Size"].object->fields["Offset"].tag=4;
  exported(&l,"RegisterView", {
    p,str("Integer"),integer_size
  },true);
  std::vector<std::string>log;
  connect(&l,a,"Entered",fn([&](lua_State*s) {
    log.push_back("A.enter1");
    assert(value(s,1).object->fields["reason"].text=="enter");
    exported(s,"EnterView", {
      b
    });
    assert(rml::workspace::stack.back()==a.object->fields["Id"].text);
    return 0;
  }));
  Value other;
  connect(&l,a,"Entered",fn([&](lua_State*s) {
    log.push_back("A.enter2");
    assert(rml::workspace::stack.back()==a.object->fields["Id"].text);
    call(s,other.object->fields["Disconnect"], {
      other
    });
    return 0;
  }));
  other=connect(&l,a,"Entered",fn([&](lua_State*) {
    log.push_back("must-not-fire");
    return 0;
  }));
  connect(&l,a,"Exited",fn([](lua_State*)->int {
    throw LuaFailure("intentional listener error");
  }));
  connect(&l,a,"Exited",fn([&](lua_State*s) {
    log.push_back("A."+value(s,1).object->fields["reason"].text);
    return 0;
  }));
  connect(&l,b,"Entered",fn([&](lua_State*) {
    log.push_back("B.enter");
    return 0;
  }));
  auto req=exported(&l,"EnterView", {
    a
  })[0];
  assert(req.object->frozen&&req.object->fields["status"].text=="queued");
  drive(&l,2);
  assert((log==std::vector<std::string> {
    "A.enter1","A.enter2","A.covered","B.enter"
  }));
  assert(rml::workspace::acks==2);
  auto stack=exported(&l,"GetViewStack", {})[0];
  assert(stack.object->array.size()==2&&stack.object->array[2].object->fields["Name"].text=="B");
  assert(stack.object->array[2].object->fields["state"].text=="active");
  auto state=exported(&l,"GetState", {
    b
  })[0];
  assert(state.object->fields["state"].text=="active");
  auto request=exported(&l,"GetRequest", {
    req
  })[0];
  assert(request.object->fields["report"].text=="report");
  auto inspect=exported(&l,"InspectPanels", {})[0];
  assert(inspect.object->fields["panels"].object->array.size()==1);
  created[0].object->fields["Enabled"]=boolean(false);
  exported(&l,"OpenPanel", {
    created[1]
  });
  assert(created[0].object->fields["Enabled"].boolean);
  exported(&l,"ClosePanel", {
    created[1]
  });
  exported(&l,"RecoverLayout", {});
  Value sig=a.object->fields["Entered"],forged=object();
  forged.object->bad_index=true;
  auto before_refs=rooted_count(&l);
  auto protected_error=call(&l,sig.object->fields["Connect"], {
    forged,fn([](lua_State*) {
      return 0;
    })
  });
  assert(protected_error.tag==0&&rooted_count(&l)==before_refs);
  const int old_largest=largest_reference(&l);
  auto fresh_connection=connect(&l,b,"Exited",fn([](lua_State*) {
    return 0;
  }));
  assert(largest_reference(&l)>old_largest);
  call(&l,fresh_connection.object->fields["Disconnect"], {
    fresh_connection
  });
  connect(&l,b,"Exited",fn([&](lua_State*s) {
    log.push_back("B."+value(s,1).object->fields["reason"].text);
    return 0;
  }));
  exported(&l,"UnregisterView", {
    b
  });
  drive(&l,1);
  assert(log.back()=="B.unregister");
  // The first owner has Unloading then Heartbeat callbacks. Its unload must
  // remove its Lua refs and leave the second owner's edit-mode driver running.
  const auto refs_before=rooted_count(&l);
  call(&l,signal_callbacks[0]);
  assert(rooted_count(&l)<refs_before);
  drive(&l,2);
  assert(!l.tasks.empty());
  call(&l,signal_callbacks[2]);
  assert(rooted_count(&l)==0);
  drive(&l,8);
  assert(l.tasks.empty());
  rml::workspace::deactivate_lua_exports();
  assert(l.stack.empty()&&constructor_calls==0);
  std::cout<<"PASS: 15 exports, RegisterPanel identity/duplicates, fail-before-call CreatePanel, protected fields/methods, exact stack results, owned public registry, readonly/foreign storage errors, monotonic refs/disconnect, event order/ACK/errors, numeric arrays, task polling, two-owner unloading cleanup\n";
}
