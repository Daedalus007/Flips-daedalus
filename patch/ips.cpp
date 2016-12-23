#include "patch.h"

namespace patch { namespace ips {
//TODO: HEAVY cleanups needed here
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define clamp(a,b,c) max(a,min(b,c))

struct ipsstudy {
	result error;
	unsigned int outlen_min;
	unsigned int outlen_max;
	unsigned int outlen_min_mem;
};

static result ips_study(struct mem patch, struct ipsstudy * study)
{
	study->error=e_broken;
	if (patch.len<8) return e_broken;
	const unsigned char * patchat=patch.ptr;
	const unsigned char * patchend=patchat+patch.len;
#define read8() ((patchat<patchend)?(*patchat++):0)
#define read16() ((patchat+1<patchend)?(patchat+=2,((patchat[-2]<<8)|patchat[-1])):0)
#define read24() ((patchat+2<patchend)?(patchat+=3,((patchat[-3]<<16)|(patchat[-2]<<8)|patchat[-1])):0)
	if (read8()!='P' ||
			read8()!='A' ||
			read8()!='T' ||
			read8()!='C' ||
			read8()!='H')
	{
		return e_broken;
	}
	unsigned int offset=read24();
	unsigned int outlen=0;
	unsigned int thisout=0;
	unsigned int lastoffset=0;
	bool w_scrambled=false;
	while (offset!=0x454F46)//454F46=EOF
	{
		unsigned int size=read16();
		if (size==0)
		{
			size=read16();
			if (!size) w_scrambled=true;
			thisout=offset+size;
			read8();
		}
		else
		{
			thisout=offset+size;
			patchat+=size;
		}
		if (offset<lastoffset) w_scrambled=true;
		lastoffset=offset;
		if (thisout>outlen) outlen=thisout;
		if (patchat>=patchend) return e_broken;
		offset=read24();
	}
	study->outlen_min_mem=outlen;
	study->outlen_max=0xFFFFFFFF;
	if (patchat+3==patchend)
	{
		unsigned int truncate=read24();
		study->outlen_max=truncate;
		if (outlen>truncate)
		{
			outlen=truncate;
			w_scrambled=true;
		}
	}
	if (patchat!=patchend) return e_broken;
	study->outlen_min=outlen;
#undef read8
#undef read16
#undef read24
	study->error=e_ok;
	if (w_scrambled) study->error=e_damaged;
	return study->error;
}

static result ips_apply_study(struct mem patch, struct ipsstudy * study, struct mem in, struct mem * out)
{
	out->ptr=NULL;
	out->len=0;
	if (study->error==e_broken) return study->error;
#define read8() (*patchat++)//guaranteed to not overflow at this point, we already checked the patch
#define read16() (patchat+=2,((patchat[-2]<<8)|patchat[-1]))
#define read24() (patchat+=3,((patchat[-3]<<16)|(patchat[-2]<<8)|patchat[-1]))
	unsigned int outlen=clamp(study->outlen_min, in.len, study->outlen_max);
	out->ptr=(uint8_t*)malloc(max(outlen, study->outlen_min_mem));
	out->len=outlen;
	
	bool anychanges=false;
	if (outlen!=in.len) anychanges=true;
	
	if (out->len>in.len)
	{
		memcpy(out->ptr, in.ptr, in.len);
		memset(out->ptr+in.len, 0, out->len-in.len);
	}
	else memcpy(out->ptr, in.ptr, outlen);
	const unsigned char * patchat=patch.ptr+5;
	unsigned int offset=read24();
	while (offset!=0x454F46)
	{
		unsigned int size=read16();
		if (size==0)
		{
			size=read16();
			if (!size) {}//no clue (fix the change detector if changing this)
			unsigned char b=read8();
			
			if (size && (out->ptr[offset]!=b || memcmp(out->ptr+offset, out->ptr+offset, size-1))) anychanges=true;
			
			memset(out->ptr+offset, b, size);
		}
		else
		{
			if (memcmp(out->ptr+offset, patchat, size)) anychanges=true;
			
			memcpy(out->ptr+offset, patchat, size);
			patchat+=size;
		}
		offset=read24();
	}
#undef read8
#undef read16
#undef read24
	
	if (study->outlen_max!=0xFFFFFFFF && in.len<=study->outlen_max) study->error=e_not_this;//truncate data without this being needed is a poor idea
	if (!anychanges) study->error=e_to_output;
	return study->error;
}

static result apply(struct mem patch, struct mem in, struct mem * out)
{
	struct ipsstudy study;
	ips_study(patch, &study);
	return ips_apply_study(patch, &study, in, out);
}

result apply(const file& patch, const file& source, file& target)
{
	struct mem patchmem = patch.mmap();
	struct mem inmem = source.mmap();
	struct mem outmem;
	result r = apply(patchmem, inmem, &outmem);
	patch.unmap(patchmem.v());
	source.unmap(inmem.v());
	target.write(outmem.v());
	free(outmem.ptr);
	return r;
}

//Known situations where this function does not generate an optimal patch:
//In:  80 80 80 80 80 80 80 80 80 80 80 80 80 80 80 80 80 80 80 80 80 80 80 80
//Out: FF FF FF FF FF FF FF FF 00 01 02 03 04 05 06 07 FF FF FF FF FF FF FF FF
//IPS: [         RLE         ] [        Copy         ] [         RLE         ]
//Possible improvement: RLE across the entire file, copy on top of that.
//Rationale: It would be a huge pain to create such a multi-pass tool if it should support writing a byte
//  more than twice, and I don't like half-assing stuff. It's also unlikely to apply to anything.


//Known improvements over LIPS:
//In:  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F
//Out: FF 01 02 03 04 05 FF FF FF FF FF FF FF FF FF FF
//LIPS:[      Copy     ] [            RLE            ]
//Mine:[] [ Unchanged  ] [            RLE            ]
//Rationale: While LIPS can break early if it finds something RLEable in the middle of a block, it's not
//  smart enough to back off if there's something unchanged between the changed area and the RLEable spot.

//In:  FF FF FF FF FF FF FF
//Out: 00 00 00 00 01 02 03
//LIPS:[   RLE   ] [ Copy ]
//Mine:[       Copy       ]
//Rationale: Mistuned heuristics in LIPS.

//It is also known that I win in some other situations. I didn't bother checking which, though.

//There are no known cases where LIPS wins over this.

static result create(struct mem sourcemem, struct mem targetmem, struct mem * patchmem)
{
	int sourcelen=sourcemem.len;
	int targetlen=targetmem.len;
	const unsigned char * source=sourcemem.ptr;
	const unsigned char * target=targetmem.ptr;
	
	patchmem->ptr=NULL;
	patchmem->len=0;
	
	if (targetlen>=16777216) return e_too_big;
	
	int offset=0;
	int outbuflen=4096;
	unsigned char * out=(uint8_t*)malloc(outbuflen);
	int outlen=0;
#define write8(val) do { out[outlen++]=(val); if (outlen==outbuflen) { outbuflen*=2; out=(uint8_t*)realloc(out, outbuflen); } } while(0)
#define write16(val) do { write8((val)>>8); write8((val)); } while(0)
#define write24(val) do { write8((val)>>16); write8((val)>>8); write8((val)); } while(0)
	write8('P');
	write8('A');
	write8('T');
	write8('C');
	write8('H');
	int lastknownchange=0;
	int lastwritten=0;
	//int forcewrite=(targetlen>sourcelen?1:0);
	while (offset<targetlen)
	{
		while (offset<targetlen && (offset<sourcelen?source[offset]:0)==target[offset]) offset++;
		//check how much we need to edit until it starts getting similar
		int thislen=0;
		int consecutiveunchanged=0;
		thislen=lastknownchange-offset;
		if (thislen<0) thislen=0;
		while (true)
		{
			int thisbyte=offset+thislen+consecutiveunchanged;
			if (thisbyte<targetlen && (thisbyte<sourcelen?source[thisbyte]:0)==target[thisbyte]) consecutiveunchanged++;
			else
			{
				thislen+=consecutiveunchanged+1;
				consecutiveunchanged=0;
			}
			if (consecutiveunchanged>=6 || thislen>65535) break;
		}
		
		//avoid premature EOF
		if (offset==0x454F46)
		{
			offset--;
			thislen++;
		}
		
		lastknownchange=offset+thislen;
		if (thislen>65535) thislen=65535;
		if (offset+thislen>targetlen) thislen=targetlen-offset;
		if (offset==targetlen) continue;
		
		//check if RLE here is worthwhile
		int byteshere;
		for (byteshere=0;byteshere<thislen && target[offset]==target[offset+byteshere];byteshere++) {}
		if (byteshere==thislen)
		{
			int thisbyte=target[offset];
			int i=0;
			while (true)
			{
				int pos=offset+byteshere+i-1;
				if (pos>=targetlen || target[pos]!=thisbyte || byteshere+i>65535) break;
				if (pos>=sourcelen || (pos<sourcelen?source[pos]:0)!=thisbyte)
				{
					byteshere+=i;
					thislen+=i;
					i=0;
				}
				i++;
			}
		}
		if ((byteshere>8-5 && byteshere==thislen) || byteshere>8)
		{
			write24(offset);
			write16(0);
			write16(byteshere);
			write8(target[offset]);
			offset+=byteshere;
			lastwritten=offset;
		}
		else
		{
			//check if we'd gain anything from ending the block early and switching to RLE
			int byteshere=0;
			int stopat=0;
			while (stopat+byteshere<thislen)
			{
				if (target[offset+stopat]==target[offset+stopat+byteshere]) byteshere++;
				else
				{
					stopat+=byteshere;
					byteshere=0;
				}
				if (byteshere>8+5 || //rle-worthy despite two ips headers
						(byteshere>8 && stopat+byteshere==thislen) || //rle-worthy at end of data
						(byteshere>8 && offset+stopat+byteshere+8 <= thislen &&
							!memcmp(&target[offset+stopat+byteshere], &target[offset+stopat+byteshere+1], 9-1)))//rle-worthy before another rle-worthy
				{
					if (stopat) thislen=stopat;
					break;//we don't scan the entire block if we know we'll want to RLE, that'd gain nothing.
				}
			}
			//don't write unchanged bytes at the end of a block if we want to RLE the next couple of bytes
			if (offset+thislen!=targetlen)
			{
				while (offset+thislen-1<sourcelen && target[offset+thislen-1]==(offset+thislen-1<sourcelen?source[offset+thislen-1]:0))
				{
					thislen--;
				}
			}
			
			//avoid infinite loops if an RLEable block starts at 'EOF'
			if (offset+thislen == 0x454F46)
			{
				if (thislen==0xFFFF) thislen--;
				else thislen++;
			}
			if (thislen>3 && !memcmp(&target[offset], &target[offset+1], thislen-1))//still worth it?
			{
				write24(offset);
				write16(0);
				write16(thislen);
				write8(target[offset]);
			}
			else
			{
				write24(offset);
				write16(thislen);
				for (int i=0;i<thislen;i++)
				{
					write8(target[offset+i]);
				}
			}
			offset+=thislen;
			lastwritten=offset;
		}
	}
	if (sourcelen<targetlen && lastwritten!=targetlen)
	{
		if (targetlen-1==0x454F46)
		{
			write24(targetlen-2);
			write16(2);
			write8(target[targetlen-2]);
			write8(target[targetlen-1]);
		}
		else
		{
			write24(targetlen-1);
			write16(1);
			write8(target[targetlen-1]);
		}
	}
	write8('E');
	write8('O');
	write8('F');
	if (sourcelen>targetlen) write24(targetlen);
#undef write
	patchmem->ptr=out;
	patchmem->len=outlen;
	if (outlen==8) return e_identical;
	return e_ok;
}

result create(const file& source, const file& target, file& patch)
{
	struct mem sourcemem = source.mmap();
	struct mem targetmem = target.mmap();
	struct mem patchmem;
	result r = create(sourcemem, targetmem, &patchmem);
	source.unmap(sourcemem.v());
	target.unmap(targetmem.v());
	patch.write(patchmem.v());
	free(patchmem.ptr);
	return r;
}

#if 0
#warning Disable this in release versions.
#include <stdio.h>

//Congratulations, you found the undocumented feature! I don't think it's useful for anything except debugging this, though.
void ips_dump(struct mem patch)
{
	if (patch.len<8)
	{
		puts("Invalid");
		return;
	}
	const unsigned char * patchat=patch.ptr;
	const unsigned char * patchend=patchat+patch.len;
#define read8() ((patchat<patchend)?(*patchat++):0)
#define read16() ((patchat+1<patchend)?(patchat+=2,((patchat[-2]<<8)|patchat[-1])):0)
#define read24() ((patchat+2<patchend)?(patchat+=3,((patchat[-3]<<16)|(patchat[-2]<<8)|patchat[-1])):0)
	if (read8()!='P' ||
			read8()!='A' ||
			read8()!='T' ||
			read8()!='C' ||
			read8()!='H')
	{
		puts("Invalid");
		return;
	}
	int blockstart=patchat-patch.ptr;
	int offset=read24();
	int outlen=0;
	int thisout=0;
	while (offset!=0x454F46)//454F46=EOF
	{
		int size=read16();
		if (size==0)
		{
			int rlelen=read16();
			thisout=offset+rlelen;
			printf("[%X] %X: %i (RLE)\n", blockstart, offset, rlelen);
			read8();
		}
		else
		{
			thisout=offset+size;
			printf("[%X] %X: %i\n", blockstart, offset, size);
			patchat+=size;
		}
		if (thisout>outlen) outlen=thisout;
		if (patchat>=patchend)
		{
			puts("Invalid");
			return;
		}
		blockstart=patchat-patch.ptr;
		offset=read24();
	}
	printf("Expand to 0x%X\n", outlen);
	if (patchat+3==patchend)
	{
		int truncate=read24();
		printf("Truncate to 0x%X\n", truncate);
	}
	if (patchat!=patchend) puts("Invalid");
#undef read8
#undef read16
#undef read24
}
#endif
}}
