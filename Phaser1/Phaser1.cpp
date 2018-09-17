#include "SC_PlugIn.h"



static InterfaceTable *ft;

struct Phaser1 : public Unit {
    //Numer of Stages
    int stages;

    //Allpass filter coefficients
    float *a;
    //Slope of a if control rate
    float *a_slope;


    float feedback;

    //Rate of a arguments (false control/scalar, true audio)
    bool *a_rate;
    //Num of audio arguments
    int num_audio_a;
    //Array containing pointers of audio rate a
    float **audio_a;

    // Last previous value for each stage
    float *prev_sig;
};


static void Phaser1_next_k(Phaser1 *unit, int inNumSamples);
static void Phaser1_next_a(Phaser1 *unit, int inNumSamples);
static void Phaser1_Ctor(Phaser1* unit);
static void Phaser1_Dtor(Phaser1* unit);


void Phaser1_Ctor(Phaser1* unit) {
    float *a,*a_slope;
    bool* a_rate;
    int stages;
    int num_audio_a=0;
    
    stages = unit->mNumInputs - 2;
    unit->stages = stages;

    unit->feedback = IN0(1);


    unit->prev_sig = (float*)RTAlloc(unit->mWorld, (stages+1)*sizeof(float));
    a = (float*)RTAlloc(unit->mWorld, stages*sizeof(float));
    a_rate = (bool*)RTAlloc(unit->mWorld, stages*sizeof(bool));
    a_slope = (float*)RTAlloc(unit->mWorld, stages*sizeof(float));

    unit->a = a;
    unit->a_slope = a_slope;
    unit->a_rate = a_rate;
    
    if(a == NULL || unit->prev_sig == NULL){
            SETCALC(ft->fClearUnitOutputs);
            ClearUnitOutputs(unit, 1);

            if(unit->mWorld->mVerbosity > -2) {
                Print("Failed to allocate memory for Phaser1 ugen.\n");
            }
            return;
    }

    Clear(stages+1, unit->prev_sig);
    Clear(stages, unit->a_slope);

    for(int i=2; i<=stages+1; ++i){
        *a_rate = (INRATE(i) == calc_FullRate);
        if(*a_rate) ++num_audio_a;
        ++a_rate;
        *(a++) = IN0(i);
        
    }

    unit->num_audio_a = num_audio_a;

    if(num_audio_a){
        SETCALC(Phaser1_next_a);
        unit->audio_a = (float**)RTAlloc(unit->mWorld, num_audio_a*sizeof(float*));
    }
    else{
        SETCALC(Phaser1_next_k);
        RTFree(unit->mWorld, unit->a_rate);
    }

    ZOUT0(0) = 0;
}

// this must be named PluginName_Dtor.
void Phaser1_Dtor(Phaser1* unit) {
    // Free the memory.
    if(unit->num_audio_a){
        RTFree(unit->mWorld, unit->audio_a);
        RTFree(unit->mWorld, unit->a_rate);
    }
    RTFree(unit->mWorld, unit->prev_sig);
    RTFree(unit->mWorld, unit->a);
    RTFree(unit->mWorld, unit->a_slope);
}

void Phaser1_next_k(Phaser1 *unit, int inNumSamples)
{
    float *in;
    float *out;

    float *a = unit->a, *a_ptr,
          *a_slope=unit->a_slope, *a_slope_ptr;
    float xa, nexta;

    float *prev_sig = unit->prev_sig, *prev_sig_ptr;
    int stages = unit->stages;

    float prev_in=0, xin=0, prev_out=0, xout=0;

    float feedback = unit->feedback,
          fb_next, fb_slope=0;
    

    fb_next = IN0(1);
    if(feedback==fb_next)
        fb_slope=0;
    else
        fb_slope = CALCSLOPE(fb_next,feedback);

    a_ptr = a;
    a_slope_ptr = a_slope;
    for(int i=2; i<=stages+1; i++){
        nexta = IN0(i);
        xa = *a_ptr;
        if(xa == nexta)
            *a_slope_ptr == 0;
        else
            *a_slope_ptr = CALCSLOPE(nexta,xa);
        ++a_ptr; ++a_slope_ptr;
    }

    in = IN(0);
    out = OUT(0);
    xout = prev_sig[stages];

    for(int t=0; t<inNumSamples; t++){

        a_ptr = a;
        a_slope_ptr = a_slope;

        prev_sig_ptr = prev_sig;

        xin = *(in++) + (feedback+=fb_slope)*xout;
        prev_in = *prev_sig_ptr;
        *prev_sig_ptr = xin;
        
        
        for(int i=0; i<stages; i++){
            xa = *a_ptr += *a_slope_ptr;
            ++a_ptr; ++a_slope_ptr;


            prev_out = *(++prev_sig_ptr);
            xout = xa * (xin + prev_out) - prev_in;

            prev_in = prev_out;
            xin = xout;
            *prev_sig_ptr = xout;

        }

        *(out++) = xout;
    }
    unit->feedback=fb_next;
}

void Phaser1_next_a(Phaser1 *unit, int inNumSamples)
{
    float *in;
    float *out;

    float *a = unit->a, *a_ptr,
          *a_slope=unit->a_slope, *a_slope_ptr,
          **audio_a=unit->audio_a, **audio_a_ptr;
    float xa, nexta;
    float *ptr;

    bool *a_rate = unit->a_rate, *a_rate_ptr;

    float *prev_sig = unit->prev_sig, *prev_sig_ptr;
    int stages = unit->stages;

    float prev_in=0, xin=0, prev_out=0, xout=0;

    float feedback = unit->feedback;
    

    a_ptr = a;
    a_slope_ptr = a_slope;
    a_rate_ptr = a_rate;
    audio_a_ptr = audio_a;
    for(int i=2; i<=stages+1; i++){
        if(*(a_rate_ptr++)){
            *(audio_a_ptr++) = IN(i);
        }
        else{
            nexta = IN0(i);
            xa = *a_ptr;
            if(xa == nexta)
                *a_slope_ptr == 0;
            else{
                *a_slope_ptr = CALCSLOPE(nexta,xa);
            }
        }
        ++a_ptr; ++a_slope_ptr;
    }

    in = IN(0);
    out = OUT(0);
    xout = prev_sig[stages];

    for(int t=0; t<inNumSamples; t++){

        a_ptr = a;
        a_slope_ptr = a_slope;
        a_rate_ptr = a_rate;
        audio_a_ptr = audio_a;

        prev_sig_ptr = prev_sig;

        xin = *(in++) + feedback*xout;
        prev_in = *prev_sig_ptr;
        *prev_sig_ptr = xin;
        
        
        for(int i=0; i<stages; i++){
            //Process a
            if(*(a_rate_ptr++)){
                xa = *((*audio_a_ptr)++);
                ++audio_a_ptr;
            }
            else
                xa = *a_ptr += *a_slope_ptr;
            ++a_ptr; ++a_slope_ptr;


            prev_out = *(++prev_sig_ptr);
            xout = xa * (xin + prev_out) - prev_in;

            prev_in = prev_out;
            xin = xout;
            *prev_sig_ptr = xout;

        }

        *(out++) = xout;
    }

}

PluginLoad(Phaser1)
{
    ft = inTable;
    // ATTENTION! This has changed!
    // In the previous examples this was DefineSimpleUnit.
    DefineDtorUnit(Phaser1);
}
