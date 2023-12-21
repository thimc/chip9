#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <keyboard.h>
#include <mouse.h>
#include <bio.h>

Image *red, *green;

Keyboardctl *kctl;
Mousectl *mctl;

#define MIN(a, b) (a < b ? a : b)

#define scr (screen->r)
#define scrx (int)(screen->r.min.x)
#define scry (int)(screen->r.min.y)
#define scrmx (int)(screen->r.max.x)
#define scrmy (int)(screen->r.max.y)
#define scrw (int)(scrmx - scrx)
#define scrh (int)(scrmy - scry)

#define WIDTH 800
#define HEIGHT 600
#define BORDER_SIZE 6

#define SCREEN_WIDTH  64
#define SCREEN_HEIGHT 32
#define MEMORY_SIZE   4096
#define STK_SIZE      16
#define NUM_REGS      16
#define NUM_KEYS      16
#define START_ADDR    0x200
#define FONTSET_SIZE  80
#define FONT_ADDR     0x50
#define PC_STEP       0x2
#define CF            0xF

#define SCREEN_X (int)((scrw/2) - (SCREEN_WIDTH*emu.scale)/2) 
#define SCREEN_Y (int)((scrh/2) - (SCREEN_HEIGHT*emu.scale)/2)

#define KEY_DEBUG      'i'
#define KEY_STEP       'o'
#define KEY_PAUSE      'p'
#define EMULATOR_SPEED 1

typedef struct emulator Emulator;
struct emulator {
	Image *screen;
	Rectangle r;
	int stack[STK_SIZE];
	char keys[NUM_KEYS];
	char gfx[SCREEN_WIDTH * SCREEN_HEIGHT];
	uchar memory[MEMORY_SIZE];
	uchar V[NUM_REGS];

	int scale, running, debug, step;
	uchar DT, ST;
	uint PC, I, SP;
};
Emulator emu;

//	1	2	3	C
//	4	5	6	D
//	7	8	9	E
//	A	0	B	F
//
//	1	2	3	4
//	Q	W	E	R
//	A	S	D	F
//	Z	X	C	V

const char keyset[NUM_KEYS] = {
	'x', '1', '2', '3',
	'q', 'w', 'e', 'a',
	's', 'd', 'z', 'c',
	'4', 'r', 'f', 'v'
};

const char fontset[FONTSET_SIZE] = {
	0xF0, 0x90, 0x90, 0x90, 0xF0, // 0
	0x20, 0x60, 0x20, 0x20, 0x70, // 1
	0xF0, 0x10, 0xF0, 0x80, 0xF0, // 2
	0xF0, 0x10, 0xF0, 0x10, 0xF0, // 3
	0x90, 0x90, 0xF0, 0x10, 0x10, // 4
	0xF0, 0x80, 0xF0, 0x10, 0xF0, // 5
	0xF0, 0x80, 0xF0, 0x90, 0xF0, // 6
	0xF0, 0x10, 0x20, 0x40, 0x40, // 7
	0xF0, 0x90, 0xF0, 0x90, 0xF0, // 8
	0xF0, 0x90, 0xF0, 0x10, 0xF0, // 9
	0xF0, 0x90, 0xF0, 0x90, 0x90, // A
	0xE0, 0x90, 0xE0, 0x90, 0xE0, // B
	0xF0, 0x80, 0x80, 0x80, 0xF0, // C
	0xE0, 0x90, 0x90, 0x90, 0xE0, // D
	0xF0, 0x80, 0xF0, 0x80, 0xF0, // E
	0xF0, 0x80, 0xF0, 0x80, 0x80  // F
};

#define OPCODE (ushort)(emu.memory[emu.PC]<<8 | emu.memory[emu.PC+1])
#define X      (uchar)((OPCODE & 0x0F00) >> 8)
#define Y      (uchar)((OPCODE & 0x00F0) >> 4)
#define N      (uchar )(OPCODE & 0x000F)
#define NN     (uchar )(OPCODE & 0x00FF)
#define NNN    (ushort)(OPCODE & 0x0FFF)
#define INST   (ushort)(OPCODE & 0xF000)

void resetemu(void);
void load_rom(char* path);
void drawpixel(Point p, char color);
void redraw_debug(void);
void redraw(void);
void resize(int x, int y);
void emouse(Mouse* m);
void err(void);
void interpret(void);
void clockproc(void *c);
void usage(void);
void threadmain(int argc, char* argv[]);


void
resetemu(void)
{
	int i;

	for(i=0; i<STK_SIZE; i++)
		emu.stack[i]=0;
	for(i=0; i<NUM_KEYS; i++)
		emu.keys[i]=0;
	for(i=0; i<(SCREEN_WIDTH*SCREEN_HEIGHT); i++)
		emu.gfx[i]=0;
	for(i=0; i<MEMORY_SIZE; i++)
		emu.memory[i]=0;
	for(i=0; i<NUM_REGS; i++)
		emu.V[i]=0;
	for(i=0; i<FONTSET_SIZE; i++)
		emu.memory[FONT_ADDR+i]=fontset[i];

	emu.I = 0;
	emu.SP = 0;
	emu.DT = 0;
	emu.ST = 0;
	emu.PC = START_ADDR;
	emu.debug = 1;
	emu.running = 1;
	emu.scale = MIN(scrw/SCREEN_WIDTH, scrh/SCREEN_HEIGHT);
	emu.r = Rect(0, 0, (emu.scale*SCREEN_WIDTH), (emu.scale*SCREEN_HEIGHT));
	emu.screen = allocimage(display, emu.r, RGB24, 1, 0x000000FF);
}

void
load_rom(char* path)
{
	Biobuf* buf;
	int c, i;

	if((buf = Bopen(path, OREAD)) == nil)
		sysfatal("bopen: %r");
	for(i=0; (c = Bgetc(buf)) != Beof; i++)
		emu.memory[START_ADDR+i] = c;
	Bterm(buf);
	print("loaded: %s\n", path);
}

void
drawpixel(Point p, char color)
{
	if(p.x<0 || p.x>=(emu.scale*SCREEN_WIDTH))
		sysfatal("draw x out of bounds");
	if(p.y<0 || p.y>=(emu.scale*SCREEN_HEIGHT))
		sysfatal("draw y out of bounds");

	draw(emu.screen,
		Rect((p.x*emu.scale), (p.y*emu.scale), (p.x*emu.scale)+emu.scale, (p.y*emu.scale)+emu.scale),
		(color ? display->white : display->black), nil, ZP);
}

void
redraw_debug(void)
{
	Point p;
	char buf[256], b[32];
	int i;

	p = Pt(scrx+SCREEN_X, scry+font->height);

	snprint(buf, sizeof buf, "%.10s opcode: 0x%.4X | inst: 0x%.4X | PC: 0x%.4X (%.4d)",
		(!emu.running?"PAUSED":"RUNNING"), OPCODE, INST, emu.PC, emu.PC-START_ADDR);
	string(screen, p, display->black, ZP, font, buf);

	//  | I: 0x%.4X (%.4d) | SP: %.4d | DT: %.4d | ST: %.4d
	//  emu.I, emu.I, emu.SP, emu.DT, emu.ST

	p.y += font->height;
	memset(buf, 0, sizeof buf);
	for(i=0; i<NUM_REGS; i++){
		snprint(b, sizeof b, "V%X=%.3d ", i, emu.V[i]);
		strcat(buf, b);
	}
	string(screen, p, display->black, ZP, font, buf);
}

void
redraw(void)
{
	Rectangle r;

	r = Rect(scrx+SCREEN_X, scry+SCREEN_Y,
		scrx+SCREEN_X+emu.r.max.x, scry+SCREEN_Y+emu.r.max.y);

	draw(screen, screen->r, display->white, nil, ZP);
	if(emu.debug)
		redraw_debug();
	draw(screen, r,	display->transparent, emu.screen, ZP);
	flushimage(display, 1);
}

void
resize(int x, int y)
{
	int fd;

	if((fd = open("/dev/wctl", OWRITE))){
		fprint(fd, "resize -dx %d -dy %d", x, y);
		close(fd);
	}
}

void
emouse(Mouse* m)
{
	USED(m);
	// TODO: Add support for a menu, maybe?
}

void
err(void)
{
	Biobuf* buf;

	if((buf = Bopen("dump", OWRITE|OTRUNC)) == nil)
		sysfatal("Bopen: %r");
	Bwrite(buf, emu.memory, sizeof emu.memory);
	Bterm(buf);

	print("unknown opcode: 0x%.4X (instruction: 0x%X) @ PC=%d\n",
		OPCODE, INST, emu.PC-START_ADDR);
	emu.running=0;
}

void
interpret(void)
{
	short i, x, y, px, py, row, bit;
	short index, p;

	switch(INST){
	case 0x0000:
		switch(NN){
		case 0x0000: // NOP [0NNN]
			break;

		case 0x00E0: // CLEAR [00E0]
			for(y=0; y<SCREEN_HEIGHT; y++){
				for(x=0; x<SCREEN_WIDTH; x++){
					emu.gfx[x+(y*SCREEN_HEIGHT)]=0;
				}
			}
			draw(emu.screen, emu.r, display->black, nil, ZP);
			break;

		case 0x00EE: // RET [00EE]
			emu.PC = emu.stack[emu.SP--];
			break;
		}
		break;

	case 0x1000: // JMP [1NNN]
		emu.PC = NNN;
		return;

	case 0x2000: // CALL NNN [2NNN]
		emu.stack[++emu.SP] = emu.PC;
		emu.PC = NNN;
		return;

	case 0x3000: // SKIP VX == NN [3XNN]
		if(emu.V[X] == NN)
			emu.PC += PC_STEP;
		break;

	case 0x4000: // SKIP VX != NN [4XNN]
		if(emu.V[X] != NN)
			emu.PC += PC_STEP;
		break;

	case 0x5000: // SKIP VX == VY [5XY0]
		if(emu.V[X] == emu.V[Y])
			emu.PC += PC_STEP;
		break;

	case 0x6000: // VX = NN [6XNN]
		emu.V[X] = NN;
		break;

	case 0x7000: // VX += NN [7XNN]
		emu.V[X] += NN;
		break;

	case 0x8000:
		switch(N){
		case 0x0000: // VX = VY [8XY0]
			emu.V[X] = emu.V[Y];
			break;

		case 0x0001: // VX |= VY [8XY1]
			emu.V[X] |= emu.V[Y];
			emu.V[CF]=0;
			break;

		case 0x0002: // VX &= VY [8XY2]
			emu.V[X] &= emu.V[Y];
			emu.V[CF]=0;
			break;

		case 0x0003: // VX ^= VY [8XY3]
			emu.V[X] ^= emu.V[Y];
			emu.V[CF]=0;
			break;

		case 0x0004: // VX += VY [8XY4]
			p = emu.V[X] + emu.V[Y];
			emu.V[X] = p;
			emu.V[CF] = (p>0xFF);
			break;

		case 0x0005: // VX -= VY [8XY5]
			p = (emu.V[X] >= emu.V[Y]);
			emu.V[X] -= emu.V[Y];
			emu.V[CF] = p;
			break;

		case 0x0006: // VX >> 1 [8XY6]
			p = (emu.V[Y] & 1);
			emu.V[X] >>= 1;
			emu.V[CF] = p;
			break;

		case 0x0007: // VX >>= 1 [8XY7]
			emu.V[X] = emu.V[Y] - emu.V[X];
			emu.V[CF] = (emu.V[Y] >= emu.V[X]);
			break;

		case 0x000E: // VX <<= 1 [8XYE]
			p = (emu.V[X] >> 7);
			emu.V[X] <<= 1;
			emu.V[CF] = p;
			break;
		}
		break;

	case 0x9000: // SKIP VX != VY [9XY0]
		if(emu.V[X] != emu.V[Y])
			emu.PC += PC_STEP;
		break;

	case 0xA000: // I = NNNN [ANNN]
		emu.I = NNN;
		break;

	case 0xB000: // JMP V0 + NNN [BNNN]
		emu.PC = NNN + emu.V[0x0];
		return;

	case 0xC000: // VX = rand() & NN [CXNN]
		srand(time(nil));
		emu.V[X] = (rand() & NN);
		break;

	case 0xD000: // DRAW [DXYNN]
		emu.V[CF]=0;
		for(row=0; row<N; row++){
			py = (emu.V[Y] + row) % SCREEN_HEIGHT;
			for(bit=0; bit<8; bit++){
				px = (emu.V[X] + (7 - bit)) % SCREEN_WIDTH;
				p = (emu.memory[emu.I + row] >> bit) & 0x01;
				index = px + (py * SCREEN_WIDTH);
				emu.V[CF] = (p && emu.gfx[index]);
				emu.gfx[index] ^= p;
				drawpixel(Pt(px, py), emu.gfx[index]);
			}
		}
		break;

	case 0xE000:
		switch(NN){
		case 0x009E: // SKIP KEY PRESS [EX9E]
			if(emu.keys[emu.V[X]]==1)
				emu.PC += PC_STEP;
			break;

		case 0x00A1: // SKIP KEY RELEASE [EXA1]
			if(emu.keys[emu.V[X]]==0)
				emu.PC += PC_STEP;
			break;
		}
		break;

	case 0xF000:
		switch(NN){
		case 0x0007: // VX = DT [FX07]
			emu.V[X] = emu.DT;
			break;

		case 0x000A: // WAIT KEY [FX0A]
			bit = -1;
			for(i=0; i<NUM_KEYS; i++){
				if(emu.keys[i] != 0){
					bit=i;
					break;
				}
			}
			if(bit != -1)
				emu.V[X] = bit;
			else
				emu.PC -= PC_STEP;
			break;

		case 0x0015: // DT = VX [FX15]
			emu.DT = emu.V[X];
			break;

		case 0x0018: // ST = VX [FX18]
			emu.ST = emu.V[X];
			break;

		case 0x001E: // I += VX [FX1E]
			emu.I += emu.V[X];
			break;

		case 0x0029: // I = FONT [FX29]
			emu.I = (emu.V[X] * 5) + FONT_ADDR;
			break;

		case 0x0033: // BCD [FX33]
			emu.memory[emu.I] = emu.V[X]/100;
			emu.memory[emu.I+1] = (emu.V[X]/10)%10;
			emu.memory[emu.I+2] = emu.V[X]%10;
			break;

		case 0x0055: // STORE V0 - VX [FX55]
			for(i=0; i<=X; i++)
				emu.memory[emu.I+i] = emu.V[i];
			break;

		case 0x0065: // LOAD V0 - VX [FX65]
			for(i=0; i<=X; i++)
				emu.V[i] = emu.memory[emu.I+i];
			break;
		}
		break;
	}

	emu.PC += PC_STEP;
	if(emu.step){
		emu.step=0;
		emu.running=0;
	}
}

void
clockproc(void *c)
{
	int i;

	threadsetname("clock");
	for(;;){
		sleep(EMULATOR_SPEED);
		sendul(c, 0);
		if(emu.PC >= MEMORY_SIZE || (!emu.running && !emu.step))
			continue;
		interpret();
		emu.DT -= (emu.DT>0);
		emu.ST -= (emu.ST>0);
		if(!emu.ST){
			// TODO: Beep!
		}

		for(i=0; i<NUM_KEYS; i++)
			emu.keys[i]=0;
	}
}

void
keyboardproc(void *)
{
	Rune r;
	int i;

	threadsetname("keyboardproc");
	for(;;){
		recv(kctl->c, &r);

		switch(r){
		case Kdel:
			threadexitsall(nil);
			break;
		case KEY_STEP:
			emu.step = 1;
			break;
		case KEY_PAUSE:
			emu.running = !emu.running;
			if(!emu.running)
				emu.step = 0;
			break;
		case KEY_DEBUG:
			emu.debug = !emu.debug;
			break;
		}
		for(i=0; i<NUM_KEYS; i++)
			emu.keys[i] = (keyset[i]==r);
	}
}

void
usage(void)
{
	fprint(2, "usage: %s rom\n", argv0);
	threadexitsall("usage");
}


void
threadmain(int argc, char* argv[])
{

	Mouse m;
	Rune k;
	Alt alts[] = {
		{ nil, &m,  CHANRCV },
		{ nil, &k,  CHANRCV },
		{ nil, nil, CHANRCV },
		{ nil, nil, CHANRCV },
		{ nil, nil, CHANEND },
	};

	ARGBEGIN{
	case 'h':
		usage();
	}ARGEND;

	if(initdraw(nil, nil, argv0) < 0)
		sysfatal("initdraw: %r");
	display->locking = 0;
	if((mctl = initmouse(nil, screen)) == nil)
		sysfatal("initmouse: %r");
	if((kctl = initkeyboard(nil)) == nil)
		sysfatal("initkeyboard: %r");

	srand(time(nil));
	resetemu();

	if(argc<1)
		usage();
	load_rom(argv[0]);

	alts[0].c = mctl->c;
	alts[1].c = kctl->c;
	alts[2].c = mctl->resizec;
	alts[3].c = chancreate(sizeof(ulong), 0);
	proccreate(clockproc, alts[3].c, 1024);
	proccreate(keyboardproc, nil, 1024);

	red = allocimage(display, Rect(0,0,1,1), RGB24, 1, 0xE47674FF);
	green = allocimage(display, Rect(0,0,1,1), RGB24, 1, 0x7DFDA4FF);

	for(;;){
		switch(alt(alts)){
		case 0:
			emouse(&m);
			break;
		case 2:
			if(getwindow(display, Refnone) < 0)
				sysfatal("getwindow: %r");
			redraw();
			break;
		case 3:
			redraw();
			break;
		}
	}
}
