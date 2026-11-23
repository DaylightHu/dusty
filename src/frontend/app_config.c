#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "app_config.h"
#include "raylib.h"
#include "../../third_party/tomlc17/tomlc17.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum { CTRL=1, SHIFT=2, ALT=4, SUPER=8, KEY_COUNT=512,
       BINDING_LIMIT=512, TEXT_LIMIT=1024, PATH_LIMIT=4096 };
static const char *actions[] = {
    "forward", "backward", "left", "right", "up", "down", "inventory", "pause",
    "command", "look_left", "look_right", "look_up", "look_down", "roll_left",
    "slot_1", "slot_2", "slot_3", "slot_4", "slot_5", "slot_6", "slot_7",
    "slot_8", "slot_9"
};
typedef struct { int key, character, mods; char text[64]; } Trigger;
typedef struct { size_t action; Trigger trigger; int line, column; } Binding;
typedef struct { AppSettings settings; Binding bindings[BINDING_LIMIT]; size_t count; } Config;
static Config current;
static char config_path[PATH_LIMIT], config_error[768];
static bool ready;
typedef struct { bool pressed[KEY_COUNT], down[KEY_COUNT]; int chars[TEXT_LIMIT], count, next, mods; } Input;
static Input physical;

static bool fail(const char *format, ...)
{
    va_list args; va_start(args,format);
    vsnprintf(config_error,sizeof(config_error),format,args); va_end(args);
    return false;
}
static int action_index(const char *name)
{
    for (size_t i=0;i<sizeof(actions)/sizeof(actions[0]);++i)
        if (!strcmp(actions[i],name)) return (int)i;
    return -1;
}
static bool equal_ci(const char *a,const char *b)
{
    for (;*a && *b;++a,++b) if (tolower((unsigned char)*a)!=tolower((unsigned char)*b)) return false;
    return *a==*b;
}
static int named_key(const char *name)
{
    static const struct { const char *name; int key; } keys[] = {
        {"Left",KEY_LEFT},{"Right",KEY_RIGHT},{"Up",KEY_UP},{"Down",KEY_DOWN},
        {"Home",KEY_HOME},{"End",KEY_END},{"PageUp",KEY_PAGE_UP},{"PageDown",KEY_PAGE_DOWN},
        {"Escape",KEY_ESCAPE},{"Esc",KEY_ESCAPE},{"Enter",KEY_ENTER},{"Tab",KEY_TAB},
        {"Backspace",KEY_BACKSPACE},{"Delete",KEY_DELETE},{"Space",KEY_SPACE},
        {"LeftShift",KEY_LEFT_SHIFT},{"RightShift",KEY_RIGHT_SHIFT}
    };
    for (size_t i=0;i<sizeof(keys)/sizeof(keys[0]);++i) if (equal_ci(name,keys[i].name)) return keys[i].key;
    if ((name[0]=='F'||name[0]=='f') && isdigit((unsigned char)name[1])) {
        char *end; long n=strtol(name+1,&end,10);
        if (!*end && n>=1 && n<=12) return KEY_F1+(int)n-1;
    }
    return 0;
}
static bool parse_trigger(const char *text,Trigger *out)
{
    *out=(Trigger){0};
    if (!*text || strlen(text)>=sizeof(out->text)) return false;
    snprintf(out->text,sizeof(out->text),"%s",text);
    char copy[64]; snprintf(copy,sizeof(copy),"%s",text);
    char *name=copy,*plus;
    while ((plus=strchr(name,'+')) && plus[1]) {
        *plus='\0'; int flag=0;
        if (equal_ci(name,"Ctrl") || equal_ci(name,"Control")) flag=CTRL;
        else if (equal_ci(name,"Shift")) flag=SHIFT;
        else if (equal_ci(name,"Alt") || equal_ci(name,"Opt")) flag=ALT;
        else if (equal_ci(name,"Super") || equal_ci(name,"Cmd")) flag=SUPER;
        else return false;
        if (flag && (out->mods&flag)) return false;
        out->mods|=flag; name=plus+1;
    }
    out->key=named_key(name);
    if (out->key) return true;
    if (name[0] && !name[1] && (unsigned char)name[0]>=33 && (unsigned char)name[0]<=126) {
        int c=(unsigned char)name[0];
        if (out->mods) {
            if (isalnum(c) || strchr("[];',./`-=\\",c)) {
                out->key=isalpha(c) ? toupper(c) : c;
                return true;
            }
            return false;
        }
        out->character=c; return true;
    }
    return false;
}
static bool add_binding(Config *config,const char *name,const char *text)
{
    int action=action_index(name);
    if (action<0) return fail("keys.world: unknown action '%s'",name);
    if (config->count==BINDING_LIMIT) return fail("too many key bindings (maximum %d)",BINDING_LIMIT);
    Trigger trigger;
    if (!parse_trigger(text,&trigger))
        return fail("keys.world.%s: invalid binding '%s'",name,text);
    bool continuous=(!strcmp(name,"forward") || !strcmp(name,"backward") || !strcmp(name,"left") || !strcmp(name,"right") ||
         !strcmp(name,"up") || !strcmp(name,"down") || !strncmp(name,"look_",5) || !strcmp(name,"roll_left"));
    if (continuous && trigger.character && !isalnum(trigger.character) && !strchr("[];',./`-=\\",trigger.character))
        return fail("keys.world.%s: held movement needs a letter, digit, unshifted key or named key",name);
    config->bindings[config->count++]=(Binding){.action=(size_t)action,.trigger=trigger};
    return true;
}
static void defaults(Config *config)
{
    *config=(Config){.settings={.vsync=true,.fps_limit=60,.msaa_samples=4,.fov=65,
        .stream_radius=5,.mouse_sensitivity=0.003f,.move_speed=5.4f}};
    static const struct { const char *action,*trigger; } world[] = {
        {"forward","w"},{"backward","s"},{"left","a"},{"right","d"},
        {"up","Space"},{"down","LeftShift"},{"down","RightShift"},
        {"inventory","e"},{"pause","Escape"},{"command","/"},
        {"look_left","Left"},{"look_right","Right"},{"look_up","Up"},
        {"look_down","Down"},{"roll_left","q"},
        {"slot_1","1"},{"slot_2","2"},{"slot_3","3"},{"slot_4","4"},{"slot_5","5"},
        {"slot_6","6"},{"slot_7","7"},{"slot_8","8"},{"slot_9","9"}
    };
    for (size_t i=0;i<sizeof(world)/sizeof(world[0]);++i) (void)add_binding(config,world[i].action,world[i].trigger);
}

const AppSettings *app_config_current(void)
{
    if (!ready) { defaults(&current); ready=true; }
    return &current.settings;
}
const char *app_config_error(void) { return config_error; }

static bool resolve_path(void)
{
    const char *override=getenv("MCMUX_CONFIG");
    if (override && *override) {
        if (strlen(override)>=sizeof(config_path)) return fail("MCMUX_CONFIG path is too long");
        bool absolute=override[0]=='/';
#ifdef _WIN32
        absolute=absolute || (override[0]=='\\' && override[1]=='\\') ||
            (isalpha((unsigned char)override[0]) && override[1]==':' && (override[2]=='/' || override[2]=='\\'));
#endif
        if (!absolute)
            return fail("MCMUX_CONFIG must be an absolute path");
        memcpy(config_path,override,strlen(override)+1); return true;
    }
    const char *base=getenv("XDG_CONFIG_HOME"),*suffix="/mcmux/config.toml";
    if (!base || !*base) {
#ifdef _WIN32
        base=getenv("APPDATA");
#else
        base=getenv("HOME"); suffix="/.config/mcmux/config.toml";
#endif
    }
    if (!base || !*base) return fail("cannot find user configuration directory; set MCMUX_CONFIG");
    int n=snprintf(config_path,sizeof(config_path),"%s%s",base,suffix);
    return (n>=0 && n<(int)sizeof(config_path)) || fail("configuration path is too long");
}
static bool config_value_error(const char *name,toml_datum_t value,const char *reason)
{
    return fail("%s:%d:%d: %s: %s",config_path,value.lineno,value.colno,name,reason);
}
static bool binding_string(toml_datum_t value)
{
    return value.type==TOML_STRING && (size_t)value.u.str.len==strlen(value.u.s);
}
static bool allowed_table(toml_datum_t table,const char *name,const char *allowed)
{
    if (table.type==TOML_UNKNOWN) return true;
    if (table.type!=TOML_TABLE) return config_value_error(name,table,"expected a table");
    for (int i=0;i<table.u.tab.size;++i) {
        char key[128]; snprintf(key,sizeof(key)," %s ",table.u.tab.key[i]);
        if ((size_t)table.u.tab.len[i]!=strlen(table.u.tab.key[i]) ||
            strpbrk(table.u.tab.key[i]," \t\r\n") || !strstr(allowed,key))
            return config_value_error(table.u.tab.key[i],table.u.tab.value[i],"unknown setting");
    }
    return true;
}
static bool number(toml_datum_t root,const char *name,double low,double high,double *out,bool integer)
{
    toml_datum_t v=toml_seek(root,name);
    if (v.type==TOML_UNKNOWN) return true;
    if (v.type!=TOML_INT64 && (integer || v.type!=TOML_FP64)) return config_value_error(name,v,"wrong numeric type");
    double n=v.type==TOML_INT64 ? (double)v.u.int64 : v.u.fp64;
    if (!isfinite(n) || n<low || n>high) return config_value_error(name,v,"value out of range");
    *out=n; return true;
}
static bool read_settings(Config *next,toml_datum_t root)
{
    /* Accept obsolete tables so existing configuration files still load. */
    if (!allowed_table(root,"config"," window  editor  output  terminal  input  world  workspace  keys ")) return false;
    static const struct { const char *table,*allowed; } fields[] = {
        {"window"," vsync  fps_limit  msaa_samples  fov "},
        {"input"," mouse_sensitivity  move_speed  scroll_lines "},
        {"world"," stream_radius "}
    };
    for (size_t i=0;i<sizeof(fields)/sizeof(fields[0]);++i)
        if (!allowed_table(toml_seek(root,fields[i].table),fields[i].table,fields[i].allowed)) return false;
    AppSettings *s=&next->settings; double n;
#define INT_FIELD(path,field,low,high) n=s->field; if (!number(root,path,low,high,&n,true)) return false; s->field=(int)n
#define FLOAT_FIELD(path,field,low,high) n=s->field; if (!number(root,path,low,high,&n,false)) return false; s->field=(float)n
    INT_FIELD("window.fps_limit",fps_limit,0,1000);
    INT_FIELD("window.msaa_samples",msaa_samples,0,8);
    if (s->msaa_samples!=0 && s->msaa_samples!=2 && s->msaa_samples!=4 && s->msaa_samples!=8)
        return config_value_error("window.msaa_samples",toml_seek(root,"window.msaa_samples"),"use 0, 2, 4 or 8");
    FLOAT_FIELD("window.fov",fov,30,120);
    INT_FIELD("world.stream_radius",stream_radius,1,16);
    FLOAT_FIELD("input.mouse_sensitivity",mouse_sensitivity,0.0001,0.1);
    FLOAT_FIELD("input.move_speed",move_speed,0.1,100);
#undef INT_FIELD
#undef FLOAT_FIELD
    toml_datum_t v=toml_seek(root,"window.vsync");
    if (v.type!=TOML_UNKNOWN) {
        if (v.type!=TOML_BOOLEAN) return config_value_error("window.vsync",v,"expected true or false");
        s->vsync=v.u.boolean;
    }
    return true;
}
static bool read_bindings(Config *next,toml_datum_t root)
{
    toml_datum_t keys=toml_seek(root,"keys");
    if (!allowed_table(keys,"keys"," workspace  normal  visual  insert  command  browser  info  output  world ")) return false;
    const char *path = "keys.world";
    toml_datum_t table = toml_seek(root, path);
    if (table.type == TOML_UNKNOWN) return true;
    if (table.type != TOML_TABLE) return config_value_error(path, table, "expected a table");
    for (int i = 0; i < table.u.tab.size; ++i) {
        const char *name = table.u.tab.key[i];
        toml_datum_t value = table.u.tab.value[i];
        if ((size_t)table.u.tab.len[i] != strlen(name))
            return config_value_error(path, value, "action must not contain NUL characters");
        int action = action_index(name);
        if (action < 0) return config_value_error(name, value, "unknown world action");
        for (size_t j = 0; j < next->count;) {
            if (next->bindings[j].action == (size_t)action)
                next->bindings[j] = next->bindings[--next->count];
            else ++j;
        }
        if (value.type != TOML_STRING && value.type != TOML_ARRAY)
            return config_value_error(name, value, "expected a string or array; [] disables the action");
        int count = value.type == TOML_ARRAY ? value.u.arr.size : 1;
        for (int j = 0; j < count; ++j) {
            toml_datum_t item = value.type == TOML_ARRAY ? value.u.arr.elem[j] : value;
            if (!binding_string(item))
                return config_value_error(name, item, "bindings must be strings without NUL characters");
            if (!add_binding(next, name, item.u.s)) {
                char detail[768];
                snprintf(detail, sizeof(detail), "%s", config_error);
                return config_value_error(path, item, detail);
            }
            next->bindings[next->count - 1].line = item.lineno;
            next->bindings[next->count - 1].column = item.colno;
        }
    }
    for (size_t i = 0; i < next->count; ++i) {
        for (size_t j = 0; j < i; ++j) {
            Binding *a = &next->bindings[i], *b = &next->bindings[j];
            if (a->trigger.key == b->trigger.key &&
                a->trigger.character == b->trigger.character &&
                a->trigger.mods == b->trigger.mods)
                return fail("%s:%d:%d: keys.world: '%s' is bound to both %s and %s",
                            config_path, a->line ? a->line : b->line,
                            a->line ? a->column : b->column, a->trigger.text,
                            actions[a->action], actions[b->action]);
        }
    }
    return true;
}
static bool read_config(void)
{
    FILE *file=fopen(config_path,"rb");
    Config next; defaults(&next);
    if (!file) {
        if (errno==ENOENT) return true;
        return fail("cannot open %s: %s",config_path,strerror(errno));
    }
    char *text=malloc(1024*1024+1);
    if (!text) { fclose(file); return fail("cannot allocate configuration buffer"); }
    size_t n=fread(text,1,1024*1024+1,file); bool bad=ferror(file)!=0;
    if (fclose(file)!=0) bad=true;
    if (bad || n>1024*1024 || memchr(text,0,n)) {
        free(text); return fail("%s: cannot read configuration (limit 1 MiB, no NUL bytes)",config_path);
    }
    toml_result_t result=toml_parse_named(text,(int)n,config_path); free(text);
    if (!result.ok) {
        bool ok=fail("%s: %s",config_path,result.errmsg); toml_free(result); return ok;
    }
    bool ok=read_settings(&next,result.toptab) && read_bindings(&next,result.toptab);
    toml_free(result);
    if (ok) current=next;
    return ok;
}
bool app_config_init(void)
{
    defaults(&current); ready=true; config_path[0]='\0'; config_error[0]='\0';
    return resolve_path() && read_config();
}
void app_input_frame(void)
{
    physical=(Input){0};
    for (int i=0;i<KEY_COUNT;++i) {
        physical.pressed[i]=IsKeyPressed(i); physical.down[i]=IsKeyDown(i);
    }
    if (physical.down[KEY_LEFT_CONTROL]||physical.down[KEY_RIGHT_CONTROL]) physical.mods|=CTRL;
    if (physical.down[KEY_LEFT_SHIFT]||physical.down[KEY_RIGHT_SHIFT]) physical.mods|=SHIFT;
    if (physical.down[KEY_LEFT_ALT]||physical.down[KEY_RIGHT_ALT]) physical.mods|=ALT;
    if (physical.down[KEY_LEFT_SUPER]||physical.down[KEY_RIGHT_SUPER]) physical.mods|=SUPER;
    int c; while ((c=GetCharPressed())!=0) if (physical.count<TEXT_LIMIT) physical.chars[physical.count++]=c;
}
static bool match(const Trigger *t,bool held)
{
    if (held) {
        int key=t->key;
        if (!key && (isalnum(t->character) || strchr("[];',./`-=\\",t->character))) key=toupper(t->character);
        if (key<=0 || key>=KEY_COUNT || !physical.down[key]) return false;
        return (physical.mods&t->mods)==t->mods;
    }
    if (t->key) return physical.mods==t->mods && physical.pressed[t->key];
    if ((physical.mods&~SHIFT)!=t->mods) return false;
    for (int i=0;i<physical.count;++i) if (physical.chars[i]==t->character) return true;
    return false;
}
bool app_input_world(const char *name,bool held)
{
    for (size_t i=0;i<current.count;++i) {
        Binding *b=&current.bindings[i];
        if (!strcmp(actions[b->action],name) && match(&b->trigger,held)) return true;
    }
    return false;
}
int app_input_char(void)
{
    return physical.next < physical.count ? physical.chars[physical.next++] : 0;
}
