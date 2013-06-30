#include <rtosc/miditable.h>

using namespace rtosc;

#define RTOSC_INVALID_MIDI 255
        
class rtosc::MidiTable_Impl
{
    public:
        MidiTable_Impl(unsigned len, unsigned elms)
            :len(len), elms(elms)
        {
            table = new MidiAddr[elms];
            for(unsigned i=0; i<elms; ++i) {
                table[i].ch   = RTOSC_INVALID_MIDI;
                table[i].ctl  = RTOSC_INVALID_MIDI;
                table[i].path = new char[len];
                table[i].conversion = NULL;
            }
            //TODO initialize all elms
            //TODO free elms
        }

        MidiAddr *begin(void) {return table;}
        MidiAddr *end(void) {return table + elms;}

        unsigned len;
        unsigned elms;
        MidiAddr *table;
};

//MidiAddr::MidiAddr(void)
//    :ch(RTOSC_INVALID_MIDI),ctl(RTOSC_INVALID_MIDI)
//{}

static void black_hole2(const char *a, const char *b)
{printf("'%s' and '%s'\n", a,b);}
static void black_hole1(const char *a)
{printf("'%s'\n", a);}

MidiTable::MidiTable(Ports &_dispatch_root)
:dispatch_root(_dispatch_root), unhandled_ch(-1), unhandled_ctl(-1),
    error_cb(black_hole2), event_cb(black_hole1)
{
    impl = new MidiTable_Impl(128,128);
    unhandled_path = new char[128];
    memset(unhandled_path, 0, 128);
}

bool MidiTable::has(uint8_t ch, uint8_t ctl) const
{
    for(auto e: *impl) {
        if(e.ch == ch && e.ctl == ctl)
            return true;
    }
    return false;
}

MidiAddr *MidiTable::get(uint8_t ch, uint8_t ctl)
{
    for(auto &e: *impl)
        if(e.ch==ch && e.ctl == ctl)
            return &e;
    return NULL;
}

const MidiAddr *MidiTable::get(uint8_t ch, uint8_t ctl) const
{
    for(auto &e:*impl)
        if(e.ch==ch && e.ctl == ctl)
            return &e;
    return NULL;
}

bool MidiTable::mash_port(MidiAddr &e, const Port &port)
{
    const char *args = index(port.name, ':');
    if(!args)
        return false;

    //Consider a path to be typed based upon the argument restrictors
    if(index(args, 'f')) {
        e.type = 'f';
        e.conversion = port.metadata;
    } else if(index(args, 'i'))
        e.type = 'i';
    else if(index(args, 'T'))
        e.type = 'T';
    else if(index(args, 'c'))
        e.type = 'c';
    else
        return false;
    return true;
}

void MidiTable::addElm(uint8_t ch, uint8_t ctl, const char *path)
{
    const Port *port = dispatch_root.apropos(path);

    if(!port || port->ports) {//missing or directory node
        error_cb("Bad path", path);
        return;
    }

    if(MidiAddr *e = this->get(ch,ctl)) {
        strncpy(e->path,path,impl->len);
        if(!mash_port(*e, *port)) {
            e->ch  = RTOSC_INVALID_MIDI;
            e->ctl = RTOSC_INVALID_MIDI;
            error_cb("Failed to read metadata", path);
        }
        return;
    }

    for(MidiAddr &e:*impl) {
        if(e.ch == RTOSC_INVALID_MIDI) {//free spot
            e.ch  = ch;
            e.ctl = ctl;
            strncpy(e.path,path,impl->len);
            if(!mash_port(e, *port)) {
                e.ch  = RTOSC_INVALID_MIDI;
                e.ctl = RTOSC_INVALID_MIDI;
                error_cb("Failed to read metadata", path);
            }
            return;
        }
    }
}

void MidiTable::check_learn(void)
{
    if(unhandled_ctl == RTOSC_INVALID_MIDI || unhandled_path[0] == '\0')
        return;
    addElm(unhandled_ch, unhandled_ctl, unhandled_path);
    unhandled_ch = unhandled_ctl = RTOSC_INVALID_MIDI;
    memset(unhandled_path, 0, sizeof(unhandled_path));
}

void MidiTable::learn(const char *s)
{
    if(strlen(s) > impl->len) {
        error_cb("String too long", s);
        return;
    }
    strcpy(unhandled_path, s);
    check_learn();
}

void MidiTable::process(uint8_t ch, uint8_t ctl, uint8_t val)
{
    const MidiAddr *addr = get(ch,ctl);
    if(!addr) {
        unhandled_ctl = ctl;
        unhandled_ch  = ch;
        check_learn();
        return;
    }

    char buffer[1024];
    switch(addr->type)
    {
        case 'f':
            rtosc_message(buffer, 1024, addr->path,
                    "f", translate(val,addr->conversion));
            break;
        case 'i':
            rtosc_message(buffer, 1024, addr->path,
                    "i", val);
            break;
        case 'T':
            rtosc_message(buffer, 1024, addr->path,
                    (val<64 ? "F" : "T"));
            break;
        case 'c':
            rtosc_message(buffer, 1024, addr->path,
                    "c", val);
    }

    event_cb(buffer);
}

Port MidiTable::learnPort(void)
{
    return Port{"learn:s", "", 0, [this](msg_t m, RtData&){
            this->learn(rtosc_argument(m,0).s);
            }};

}

Port MidiTable::registerPort(void)
{
    return Port{"register:iis","", 0, [this](msg_t m,RtData&){
            const char *pos = rtosc_argument(m,2).s;
            while(*pos) putchar(*pos++);
            this->addElm(rtosc_argument(m,0).i,rtosc_argument(m,1).i,rtosc_argument(m,2).s);}};
}

//TODO generalize to an addScalingFunction() system
float MidiTable::translate(uint8_t val, const char *meta_)
{
    //Allow for middle value to be set
    //TODO consider the centered trait for this op
    float x = val!=64.0 ? val/127.0 : 0.5;

    Port::MetaContainer meta(meta_);

    if(!meta["min"] || !meta["max"] || !meta["scale"]) {
        fprintf(stderr, "failed to get properties\n");
        return 0.0f;
    }

    const float min   = atof(meta["min"]);
    const float max   = atof(meta["max"]);
    const char *scale = meta["scale"];

    if(!strcmp(scale,"linear"))
        return x*(max-min)+min;
    else if(!strcmp(scale,"logarithmic")) {
        const float b = log(min);
        const float a = log(max)-b;
        return expf(a*x+b);
    }

    return 0.0f;
}