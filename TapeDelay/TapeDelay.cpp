#include <queue>
#include "SC_PlugIn.h"

static InterfaceTable *ft;

struct SpeedChange {
    double speed;
    int stop;
    double dsamp;
};

struct InterpolationUnit
{
	static const int minDelaySamples = 1;
};

struct CubicInterpolationUnit
{
	static const int minDelaySamples = 2;
};

struct DelayUnit : public Unit
{
	float *m_dlybuf;

	double m_dsamp;
    float m_fdelaylen;
	float m_delaytime, m_maxdelaytime;
	long m_iwrphase, m_idelaylen, m_mask;
	long m_numoutput;
};

struct TapeDelay : public DelayUnit, InterpolationUnit
{
    std::queue<SpeedChange>* m_speedchanges;
    SpeedChange m_nextspeedchange;
    double m_dsampinc;
    double m_speed;
};

void TapeDelay_next_z(TapeDelay *unit, int inNumSamples);
void TapeDelay_next(TapeDelay *unit, int inNumSamples);

static bool DelayUnit_AllocDelayLine(DelayUnit *unit, const char * className)
{
	long delaybufsize = (long)ceil(unit->m_maxdelaytime * SAMPLERATE + 1.f);
	delaybufsize = delaybufsize + BUFLENGTH;
	delaybufsize = NEXTPOWEROFTWO(delaybufsize);  // round up to next power of two
	unit->m_fdelaylen = unit->m_idelaylen = delaybufsize;

	if (unit->m_dlybuf)
		RTFree(unit->mWorld, unit->m_dlybuf);
	unit->m_dlybuf = (float*)RTAlloc(unit->mWorld, delaybufsize * sizeof(float));

#if 0 // for debugging we may want to fill the buffer with nans
	std::fill_n(unit->m_dlybuf, delaybufsize, std::numeric_limits<float>::signaling_NaN());
#endif

	if (unit->m_dlybuf == NULL) {
		SETCALC(ft->fClearUnitOutputs);
		ClearUnitOutputs(unit, 1);

		if(unit->mWorld->mVerbosity > -2)
			Print("Failed to allocate memory for %s ugen.\n", className);
	}

	unit->m_mask = delaybufsize - 1;
	return (unit->m_dlybuf != NULL);
}


template <typename Unit>
static float CalcDelay(Unit *unit, float delaytime)
{
	float minDelay = Unit::minDelaySamples;
	float next_dsamp = delaytime * (float)SAMPLERATE;
	return sc_clip(next_dsamp, minDelay, unit->m_fdelaylen);
}

template <typename Unit>
static bool DelayUnit_Reset(Unit *unit, const char * className)
{
	unit->m_maxdelaytime = ZIN0(1);
	unit->m_delaytime = ZIN0(2);
	unit->m_dlybuf = 0;

	if (!DelayUnit_AllocDelayLine(unit, className))
		return false;

	unit->m_dsamp = (double) CalcDelay(unit, unit->m_delaytime);

	unit->m_numoutput = 0;
	unit->m_iwrphase = 0;
    unit->m_nextspeedchange.stop = -1;
    unit->m_nextspeedchange.speed = 1.;
    unit->m_dsampinc = 0.;
    unit->m_speed = 1.;
    unit->m_speedchanges = new std::queue<SpeedChange>;
	return true;
}


void DelayUnit_Dtor(DelayUnit *unit)
{
	RTFree(unit->mWorld, unit->m_dlybuf);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////


void Delay_next_0(DelayUnit *unit, int inNumSamples)
{
	float *out = OUT(0);
	const float *in = IN(0);

	memcpy(out, in, inNumSamples * sizeof(float));
}

void Delay_next_0_nop(DelayUnit *unit, int inNumSamples)
{}

static bool DelayUnit_init_0(DelayUnit *unit)
{
	if (INRATE(2) == calc_ScalarRate && ZIN0(2) == 0) {
		if (ZIN(0) == ZOUT(0))
			SETCALC(Delay_next_0_nop);
#ifdef NOVA_SIMD
		else if (boost::alignment::is_aligned( BUFLENGTH, 16 ))
			SETCALC(Delay_next_0_nova);
#endif
		else
			SETCALC(Delay_next_0);

		ZOUT0(0) = ZIN0(0);
		return true;
	} else
		return false;
}


enum {
	initializationComplete,
	initializationIncomplete
};

template <typename Delay>
static int Delay_Ctor(Delay *unit, const char *className)
{
	bool allocationSucessful = DelayUnit_Reset(unit, className);
	if (!allocationSucessful)
		return initializationComplete;

	// optimize for a constant delay of zero
	if (DelayUnit_init_0(unit))
		return initializationComplete;
	return initializationIncomplete;
}


void TapeDelay_Ctor(TapeDelay *unit)
{
	if (Delay_Ctor(unit, "TapeDelay") == initializationComplete)
		return;

	//if (INRATE(2) == calc_FullRate)
    //	SETCALC(TapeDelay_next_a_z);
	//else
		SETCALC(TapeDelay_next_z);
	ZOUT0(0) = 0.f;
}



template <bool Checked = false>
struct TapeDelay_helper
{
	static const bool checked = false;

	static inline void perform(const float *& in, float *& out, float * bufData,
							   long & iwrphase, long idsamp, float frac, long mask)
	{
		bufData[iwrphase & mask] = ZXP(in);
		long irdphase1 = iwrphase - idsamp;
		long irdphase2 = irdphase1 - 1;
		long irdphase3 = irdphase1 - 2;
		long irdphase0 = irdphase1 + 1;
		float d0 = bufData[irdphase0 & mask];
		float d1 = bufData[irdphase1 & mask];
		float d2 = bufData[irdphase2 & mask];
		float d3 = bufData[irdphase3 & mask];
		ZXP(out) = cubicinterp(frac, d0, d1, d2, d3);
		iwrphase++;
	}
};

template <>
struct TapeDelay_helper<true>
{
	static const bool checked = true;

	static inline void perform(const float *& in, float *& out, float * bufData,
							   long & iwrphase, long idsamp, float frac, long mask)
	{
		long irdphase1 = iwrphase - idsamp;
		long irdphase2 = irdphase1 - 1;
		long irdphase3 = irdphase1 - 2;
		long irdphase0 = irdphase1 + 1;

		bufData[iwrphase & mask] = ZXP(in);
		if (irdphase0 < 0) {
			ZXP(out) = 0.f;
		} else {
			float d0, d1, d2, d3;
			if (irdphase1 < 0) {
				d1 = d2 = d3 = 0.f;
				d0 = bufData[irdphase0 & mask];
			} else if (irdphase2 < 0) {
				d1 = d2 = d3 = 0.f;
				d0 = bufData[irdphase0 & mask];
				d1 = bufData[irdphase1 & mask];
			} else if (irdphase3 < 0) {
				d3 = 0.f;
				d0 = bufData[irdphase0 & mask];
				d1 = bufData[irdphase1 & mask];
				d2 = bufData[irdphase2 & mask];
			} else {
				d0 = bufData[irdphase0 & mask];
				d1 = bufData[irdphase1 & mask];
				d2 = bufData[irdphase2 & mask];
				d3 = bufData[irdphase3 & mask];
			}
			ZXP(out) = cubicinterp(frac, d0, d1, d2, d3);
		}
		iwrphase++;
	}
};



static int counter=0;


/* template function to generate delay ugen function, control-rate delay time */
template <typename PerformClass,
		  typename DelayX
		 >
inline void TapeDelayX_perform(DelayX *unit, int inNumSamples, UnitCalcFunc resetFunc)
{
	float *out = ZOUT(0);
	const float *in = ZIN(0);
	float delaytime = ZIN0(2);

	float *dlybuf = unit->m_dlybuf;
	long iwrphase = unit->m_iwrphase;
	double dsamp = unit->m_dsamp;
	long mask = unit->m_mask;
    
    double dsampinc = unit->m_dsampinc;
    double speed = unit->m_speed;
    int stop = unit->m_nextspeedchange.stop;

	if (delaytime != unit->m_delaytime) {
        double nspeed = unit->m_delaytime/delaytime;
        int nstop = iwrphase;
        speed *= nspeed;
        dsampinc = 1. - speed;
        //Print("%f - %f - %f\n", dsamp/SAMPLERATE, ndsampinc, dsampinc);
        //Print("add %f - %f\n", ndsampinc, dsampinc);
        if (stop == -1){
            //Print("%f - %f\n", delaytime, unit->m_delaytime);
            stop = nstop;
            unit->m_nextspeedchange.speed = nspeed;
            unit->m_nextspeedchange.stop = nstop;
            double ndsamp = unit->m_nextspeedchange.dsamp = (double) CalcDelay(unit,delaytime);
            //Print("start: %d - %f - %f -- %f, %f, %f\n", iwrphase, dsamp, dsamp+dsampinc, ndsamp, dsampinc, iwrphase-dsamp);
            counter=0;
        }
        else{
            SpeedChange spd = {nspeed, nstop, CalcDelay(unit,delaytime)};
            unit->m_speedchanges->push(spd);
        }
        unit->m_delaytime = delaytime;
    }
    //Print("%d - %f\n", unit->m_curspeed.stop, iwrphase-dsamp);


    LOOP1(inNumSamples,
        counter+=1;
        dsamp += dsampinc;
        long idsamp = (long)dsamp;
        if(stop != -1 && iwrphase-idsamp >= stop){
            if( unit->m_speedchanges->empty()){
                speed = 1.f;
                dsampinc = 0.f;
                dsamp = unit->m_nextspeedchange.dsamp;
                idsamp = (long)dsamp;
                stop = -1;
                unit->m_nextspeedchange.speed = 1.f;
                unit->m_nextspeedchange.stop = -1;
            }
            else{
                speed /= unit->m_nextspeedchange.speed;
                dsampinc = 1. - speed;

                unit->m_nextspeedchange = unit->m_speedchanges->front();
                unit->m_speedchanges->pop();
                stop = unit->m_nextspeedchange.stop;
            }
        }
        float frac = dsamp - idsamp;
        PerformClass::perform(in, out, dlybuf, iwrphase, idsamp, frac, mask);
            
    );
    unit->m_dsamp = dsamp;
    unit->m_dsampinc = dsampinc;
    unit->m_speed = speed;

	unit->m_iwrphase = iwrphase;

	if (PerformClass::checked) {
		unit->m_numoutput += inNumSamples;
		if (unit->m_numoutput >= unit->m_idelaylen)
			unit->mCalcFunc = resetFunc;
	}
}


template <bool checked>
inline void TapeDelay_perform(TapeDelay *unit, int inNumSamples)
{
	TapeDelayX_perform<TapeDelay_helper<checked> >(unit, inNumSamples, (UnitCalcFunc)TapeDelay_next);
}

void TapeDelay_next(TapeDelay *unit, int inNumSamples)
{
	TapeDelay_perform<false>(unit, inNumSamples);
}

void TapeDelay_next_z(TapeDelay *unit, int inNumSamples)
{
	TapeDelay_perform<true>(unit, inNumSamples);
}


PluginLoad(TapeDelay)
{
    ft = inTable;
    // ATTENTION! This has changed!
    // In the previous examples this was DefineSimpleUnit.
#define DefineDelayUnit(name) \
	(*ft->fDefineUnit)(#name, sizeof(name), (UnitCtorFunc)&name##_Ctor, \
	(UnitDtorFunc)&DelayUnit_Dtor, 1);
    DefineDelayUnit(TapeDelay);
}




