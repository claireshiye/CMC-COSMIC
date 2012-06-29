/* -*- linux-c -*- */

#include <sys/times.h>
#include <zlib.h>
#include <gsl/gsl_rng.h>
#include <gsl/gsl_errno.h>
#include <gsl/gsl_roots.h>
#include <fitsio.h>
#include <string.h>
#include "common/fitslib.h"
#include "common/taus113-v2.h"
#include "fewbody-0.24/fewbody.h"
#include "cmc_core.h"

#ifdef USE_MPI
#include "cmc_mpi.h"
#endif

// This automatic SVN versioning only updates the version of this file.
//#define CMCVERSION "$Revision$"
//#define CMCDATE "$Date$"
#define CMCNICK "SVN"
#define CMCPRETTYNAME "ClusterMonteCarlo"

/*************************** Parameters ******************************/
/* Large number, but still SF_INFINITY - 1 <> SF_INFINITY */
#define SF_INFINITY 1.0e10
/* Radius of the zeroth star so that 1/star[0].r is finite */
#define ZERO 1.0e-20

#define PI 3.14159265358979323

#define N_TRY 50000

#ifndef EXPERIMENTAL
#define AVEKERNEL 20
#endif

#define MSUN 1.989e+33
#define SOLAR_MASS MSUN
#define RSUN 6.9599e10
#define G 6.67259e-8
#define KPC 3.0857e+21
#define YEAR 3.155693e+7
#define AU 1.496e+13
#define PARSEC 3.0857e+18

/* defining maximum pericenters for "strong" interactions */
/* rperi = XCOLL * (R_1 + R_2) */
/* We'll set XCOLL to 4 here, as a generous estimate of the tidal capture + merger cross section.  XCOLL is used
   only to estimate the timestep, so it doesn't have to be commpletely accurate.  The real cross section is still
   used when collisions are checked for.  */
#define XCOLLTC 4.0 /* tidal capture */
#define XCOLLSS 1.5 /* simple sticky spheres */
/* rperi = XBS * a */
#define XBS 2.0
/* rperi = XBB * (a_1 + a_2) */
#define XBB 2.0
/* hard soft boundary XHS=vorb/W */
#define XHS 0.7
/* safety factor used in the black hole accretion routine */
/* by Freitag & Benz (2002) */
#define CSAFE 0.2

/* extra star type */
#define NOT_A_STAR (-100)

/*Sourav: required for the preaging when STAR_AGING_SCHEME is emabled in Solar mass*/
#define PREAGING_MASS 5.0

#define MAX_STRING_LENGTH 2048




/* binaries */
typedef struct{
	long id1; /* unique id of star 1 */
	long id2; /* unique id of star 2 */
	double rad1; /* radius of star 1 */
	double rad2; /* radius of star 2 */
	double m1; /* mass of star 1 */
	double m2; /* mass of star 2 */
	double Eint1; /* internal energy of star 1 */
	double Eint2; /* internal energy of star 2 */
	double a; /* semimajor axis */
	double e; /* eccentricity */
	int inuse; /* whether or not binary exists */
	int bse_kw[2]; /* star types */
	double bse_mass0[2]; /* initial masses */
	double bse_mass[2]; /* masses */
	double bse_radius[2]; /* radii */
	double bse_lum[2]; /* luminosity */
	double bse_massc[2];
	double bse_radc[2];
	double bse_menv[2];
	double bse_renv[2];
	double bse_ospin[2]; /* original spin */
	double bse_epoch[2];
	double bse_tms[2];
	double bse_tphys; /* physical time */
	double bse_tb; /* binary orbital period */
	double bse_bcm_dmdt[2]; /* mass transfer rate for each star [bse_get_bcm(i,14), bse_get_bcm(i,28)] */
	double bse_bcm_radrol[2]; /* radius/roche_lobe_radius for each star [bse_get_bcm(i,15), bse_get_bcm(i,29)] */
	//Sourav:toy rejuvenation variables
	double lifetime_m1; /*Sourav: lifetime of star1*/
	double lifetime_m2; /*Sourav: lifetime of star2*/
	double createtime_m1; /*Sourav: createtime of star1*/
	double createtime_m2; /*Sourav: createtime of star2*/
} binary_t;

struct star_coords {
  long index;
  long field_index;
  double r;
  double vr;
  double vt;
  double E, J;
  double pot;
};

/* single stars/objects */
typedef struct{
	/* dynamical evolution variables */
	double r;  /* radial coordinate */
	double vr; /* radial velocity */
	double vt; /* tangential velocity */
	double m; /* mass */
	double E; /* kinetic plus potential energy per unit mass */
	double J; /* angular momentum per unit mass */
	double EI; /* "intermediate" energy per unit mass */
	double Eint; /* internal energy (due to collisions, e.g.) */
	double rnew; /* new radial coordinate */
	double vrnew; /* new radial velocity */
	double vtnew; /* new tangential velocity */
	double rOld; /* ? */
	double X; /* ? */
	double Y; /* random variable that must be stored */
	double r_peri; /* pericenter distance */
	double r_apo; /* apocenter distance */
	double phi; /* value of potential at position of star (only updated at end of timestep) */
	long   interacted; /* whether or not the star has undergone a strong interaction (i.e., not relaxation) */
	long   binind; /* index to the binary */
	long   id; 	/* the star's unique identifier */
	double rad; /* radius */
	double Uoldrold, Uoldrnew; /* variables for Stodolkiewicz */
	double vtold, vrold;       /* energy conservation scheme  */
	/* TODO: stellar evolution variables */
	double se_mass;
	int se_k;
	double se_mt;
	double se_ospin;
	double se_epoch;
	double se_tphys;
	double se_radius;
	double se_lum;
	double se_mc;
	double se_rc;
	double se_menv;
	double se_renv;
	double se_tms;
	//Sourav: toy rejuvenation variables
	double createtime, createtimenew, createtimeold;
	double lifetime, lifetimeold, lifetimenew;
} star_t;

struct CenMa{
	double m;
	double m_new;
	double E;
};

struct get_pos_str {
	double max_rad;
	double phi_rtidal;
	double phi_zero;
	long N_LIMIT;
	int taskid;
	struct CenMa CMincr;
	gsl_rng *thr_rng;
};

// This is a total hack for including parameter documentation
typedef struct{
#define PARAMDOC_SAMPLESIZE "no.of sample keys contributed per processor for sample sort--binary interactions. Applicable only for the parallel version"
	int SAMPLESIZE;
#define PARAMDOC_BINBIN "toggles binary--binary interactions (0=off, 1=on)"
	int BINBIN;
#define PARAMDOC_BINSINGLE "toggles binary--single interactions (0=off, 1=on)"
	int BINSINGLE;
#define PARAMDOC_STREAMS "to run the serial version with the given number of random streams - primarily used to mimic the parallel version running with the same no.of processors"
	int STREAMS;
#define PARAMDOC_SNAPSHOTTING "toggles output snapshotting (0=off, 1=on)"
	int SNAPSHOTTING;
#define PARAMDOC_SNAPSHOT_DELTAT "snapshotting time interval (FP units)"
	int SNAPSHOT_DELTAT;
#define PARAMDOC_SNAPSHOT_DELTACOUNT "snapshotting interval in time steps"
	int SNAPSHOT_DELTACOUNT;
#define PARAMDOC_SNAPSHOT_CORE_COLLAPSE "output extra snapshotting information during core collapse (0=off, 1=on)"
        int SNAPSHOT_CORE_COLLAPSE;
#define PARAMDOC_SNAPSHOT_CORE_BOUNCE "output extra snapshotting information during core bounce (0=off, 1=on)"
        int SNAPSHOT_CORE_BOUNCE;
#define PARAMDOC_SNAPSHOT_WINDOWS "Output extra snapshots within time windows. \nThe format is start_w0,step_w0,end_w0;start_w1,step_w1,stop_w1 ... etc." 
        int SNAPSHOT_WINDOWS;
#define PARAMDOC_SNAPSHOT_WINDOW_UNITS "Units used for time window parameters. Possible choices: Gyr, Trel, and Tcr"
        int SNAPSHOT_WINDOW_UNITS;
#define PARAMDOC_IDUM "random number generator seed"
	int IDUM;
#define PARAMDOC_INPUT_FILE "input FITS file"
	int INPUT_FILE;
#define PARAMDOC_MASS_PC "mass fractions for Lagrange radii"
	int MASS_PC;
#define PARAMDOC_MASS_PC_BH_INCLUDE "Shall the central black hole be included for the calculation of the Lagrange radii? (1=yes, 0=no)"
        int MASS_PC_BH_INCLUDE;
#define PARAMDOC_MASS_BINS "mass ranges for calculating derived quantities"
	int MASS_BINS;
#define PARAMDOC_MINIMUM_R "radius of central mass"
	int MINIMUM_R;
#define PARAMDOC_STOPATCORECOLLAPSE "stop calculation at core collapse (0=no, 1=yes)"
	int STOPATCORECOLLAPSE;
#define PARAMDOC_NUM_CENTRAL_STARS "number of central stars used to calculate certain averages"
	int NUM_CENTRAL_STARS;
#define PARAMDOC_PERTURB "perform dynamical perturbations on objects (0=off, 1=on)"
	int PERTURB;
#define PARAMDOC_RELAXATION "perform two-body relaxation (0=off, 1=on)"
	int RELAXATION;
#define PARAMDOC_THETASEMAX "maximum super-encounter scattering angle (radians)"
	int THETASEMAX;
#define PARAMDOC_STELLAR_EVOLUTION "stellar evolution (0=off, 1=on)"
	int STELLAR_EVOLUTION;
#define PARAMDOC_TIDAL_TREATMENT "choose the tidal cut-off criteria (0=radial criteria, 1=Giersz energy criteria)"
	int TIDAL_TREATMENT;
#define PARAMDOC_SS_COLLISION "perform physical stellar collisions (0=off, 1=on)"
	int SS_COLLISION;
#define PARAMDOC_TIDAL_CAPTURE "allow for tidal capture in single-single interactions, including Lombardi, et al. (2006) collisional binary formation mechanism (0=off, 1=on)"
	int TIDAL_CAPTURE;
	//Sourav: toy rejuvenation flags
#define PARAMDOC_STAR_AGING_SCHEME "the aging scheme of the stars (0=infinite age of all stars, 1=rejuvenation, 2=zero lifetime of collision stars, 3=arbitrary lifetime)"
	int STAR_AGING_SCHEME;
#define PARAMDOC_PREAGING "preage the cluster (0=off, 1=on)"
	int PREAGING;
#define PARAMDOC_TERMINAL_ENERGY_DISPLACEMENT "energy change calculation stopping criterion"
	int TERMINAL_ENERGY_DISPLACEMENT;
#define PARAMDOC_T_MAX "maximum integration time (FP units)"
	int T_MAX;
#define PARAMDOC_T_MAX_PHYS "maximum integration time (Gyr)"
	int T_MAX_PHYS;
#define PARAMDOC_T_MAX_COUNT "maximum number of time steps"
	int T_MAX_COUNT;
#define PARAMDOC_MAX_WCLOCK_TIME "maximum wall clock time (seconds)"
	int MAX_WCLOCK_TIME;
#define PARAMDOC_WIND_FACTOR "stellar evolution wind mass loss factor (0.5-2)"
	int WIND_FACTOR;
#define PARAMDOC_GAMMA "gamma in Coulomb logarithm"
	int GAMMA;
#define PARAMDOC_SEARCH_GRID "search grid (0=off, 1=on)"
        int SEARCH_GRID;
#define PARAMDOC_SG_STARSPERBIN "number of stars that should ideally be in each search bin"
        int SG_STARSPERBIN;
#define PARAMDOC_SG_MAXLENGTH "maximum length of the search grid"
        int SG_MAXLENGTH;
#define PARAMDOC_SG_MINLENGTH "minimum length of the search grid"
        int SG_MINLENGTH;
#define PARAMDOC_SG_POWER_LAW_EXPONENT "slope of the assumed power-law for r(N), where N is the number of stars within r. (0.5 hard coded)"
        int SG_POWER_LAW_EXPONENT;
#define PARAMDOC_SG_MATCH_AT_FRACTION "fraction frac that adjusts the constant factor in the power-law for r(N) such that r_pl(frac*N_tot)=r(frac*N_tot) (0.5)"
        int SG_MATCH_AT_FRACTION;
#define PARAMDOC_SG_PARTICLE_FRACTION "frac_p that defines the maximum Np= frac_p*N_tot for which r(N<Np) can be reasonably approximated as a power-law (0.95)"
        int SG_PARTICLE_FRACTION;
#define PARAMDOC_BH_LOSS_CONE "perform loss-cone physics for central black hole (0=off, 1=on)"
        int BH_LOSS_CONE;
#define PARAMDOC_BH_R_DISRUPT_NB "central black hole disruption radius (N-body units)"
        int BH_R_DISRUPT_NB;
#define PARAMDOC_FORCE_RLX_STEP "force a relaxation step (useful when RELAXATION=0) (0=off, 1=on)"
        int FORCE_RLX_STEP; 
#define PARAMDOC_DT_HARD_BINARIES "calculate the binary interaction time steps by only considering hard binaries (0=off, 1=on)"
        int DT_HARD_BINARIES; 
#define PARAMDOC_HARD_BINARY_KT "The minimum binary binding energy (in units of kT) for a binary to be considered 'hard' for the time step calculation."
        int HARD_BINARY_KT; 
#ifdef EXPERIMENTAL
#define PARAMDOC_BH_LC_FDT "sub time step size on which the code tries to approximately advance particles that have a MC time step larger than BH_LC_FDT times the local relaxation time. In some versions of the code the particles are literally advanced while in other a simple scaling is used. None of them really work. (0)"
        int BH_LC_FDT;
#define PARAMDOC_AVEKERNEL "one half the number of stars over which to average certain quantities"
        int AVEKERNEL;
#endif
#define PARAMDOC_APSIDES_PRECISION "absolute precision of the roots of vr^2 for the numerical root finding algorithm."
        int APSIDES_PRECISION;
#define PARAMDOC_APSIDES_MAX_ITER "maximum number of iterations to find the roots of vr^2 numerically"
        int APSIDES_MAX_ITER;
#define PARAMDOC_APSIDES_CONVERGENCE "difference of the roots between two consecutive iterations of the numerical root finding algorithm below which the result is considered to be converged."
        int APSIDES_CONVERGENCE;
#define PARAMDOC_CIRC_PERIOD_THRESHOLD "A threshold value for the difference between apastron and periastron below which an orbit is considered to be circular. Currently this is only used for the period calculation in the loss-cone routine."
        int CIRC_PERIOD_THRESHOLD;
#define PARAMDOC_WRITE_STELLAR_INFO "Write out information about stellar evolution for each single and binary star, (0=off, 1=on)"
        int WRITE_STELLAR_INFO;
#define PARAMDOC_WRITE_RWALK_INFO "Write out information about the random walk in J-space around the central black hole, (0=off, 1=on)"
        int WRITE_RWALK_INFO;
#define PARAMDOC_WRITE_EXTRA_CORE_INFO "Write out information about cores that are defined differently from the standard (0=off, 1=on)"
        int WRITE_EXTRA_CORE_INFO;
#define PARAMDOC_CALCULATE10 "Write out information about 10\% lagrange radius (0=off, 1=on)"
        int CALCULATE10;
#define PARAMDOC_OVERWRITE_RVIR "Instead of reading the virial radius from the fits file use this value [pc]"
        int OVERWRITE_RVIR;
#define PARAMDOC_OVERWRITE_Z "Instead of reading the metallicity from the fits file use this value"
        int OVERWRITE_Z;
#define PARAMDOC_OVERWRITE_RTID "Instead of reading the tidal radius from the fits file use this value [pc]"
        int OVERWRITE_RTID;
#define PARAMDOC_OVERWRITE_MCLUS "Instead of reading the cluster mass from the fits file use this value [Msun]"
        int OVERWRITE_MCLUS;
#define PARAMDOC_BSE_NETA "neta > 0 turns wind mass-loss on, is also the Reimers mass-loss coefficent (neta*4x10^-13: 0.5 normally)."
	int BSE_NETA;
#define PARAMDOC_BSE_BWIND "bwind is the binary enhanced mass-loss parameter (inactive for single, and normally 0.0 anyway)."
	int BSE_BWIND;
#define PARAMDOC_BSE_HEWIND "hewind is a helium star mass-loss factor (0.5 normally)."
	int BSE_HEWIND;
#define PARAMDOC_BSE_WINDFLAG "windflag sets which wind prescription to use (0=BSE, 1=StarTrack, 2=Vink)."
	int BSE_WINDFLAG;
#define PARAMDOC_BSE_ALPHA1 "alpha1 is the common-envelope efficiency parameter (1.0 or 3.0 depending upon what you like and if lambda is variable)"
	int BSE_ALPHA1;
#define PARAMDOC_BSE_LAMBDA "labmda is the stellar binding energy factor for common-envelope evolution (0.5; +'ve allows it to vary, -'ve holds it constant at that value always)."
	int BSE_LAMBDA;
#define PARAMDOC_BSE_CEFLAG "ceflag sets CE prescription used. = 0 sets Tout et al. method, = 3 activates de Kool common-envelope models (normally 0)."
	int BSE_CEFLAG;
#define PARAMDOC_BSE_TFLAG "tflag > 0 activates tidal circularisation (1)."
	int BSE_TFLAG;
#define PARAMDOC_BSE_IFFLAG "ifflag > 0 uses WD IFMR of HPE, 1995, MNRAS, 272, 800 (0)."
	int BSE_IFFLAG;
#define PARAMDOC_BSE_WDFLAG "wdflag > 0 uses modified-Mestel cooling for WDs (1)."
	int BSE_WDFLAG;
#define PARAMDOC_BSE_BHFLAG "bhflag > 0 allows velocity kick at BH formation (1)."
	int BSE_BHFLAG;
#define PARAMDOC_BSE_NSFLAG "nsflag > 0 takes NS/BH mass distribution of Belczynski et al. 2002, ApJ, 572, 407 (1)."
	int BSE_NSFLAG;
#define PARAMDOC_BSE_MXNS "mxns is the maximum NS mass (1.8, nsflag=0; 3.0, nsflag=1)."
	int BSE_MXNS;
#define PARAMDOC_BSE_BCONST "bconst is the magnetic field decay timescale (-3000, although value and decay rate not really established...)."
	int BSE_BCONST;
#define PARAMDOC_BSE_CK "CK is an accretion induced field decay constant (-1000, although again this isn't well established...)."
	int BSE_CK;
#define PARAMDOC_BSE_IDUM "idum in the random number seed used by kick.f and setting initial pulsar spin period and magnetic field."
	int BSE_IDUM;
#define PARAMDOC_BSE_SIGMA "sigma is the Maxwellian dispersion for SN kick speeds (265 km/s)."
	int BSE_SIGMA;
#define PARAMDOC_BSE_BETA "beta is the wind velocity factor: proprotinal to vwind^2 (1/8)."
	int BSE_BETA;
#define PARAMDOC_BSE_EDDFAC "eddfac is Eddington limit factor for mass transfer (1.0, 10 turns Eddlimitation off)."
	int BSE_EDDFAC;
#define PARAMDOC_BSE_GAMMA "gamma is the angular momentum factor for mass lost during Roche (-1.0, see evolv2.f for more details)."
	int BSE_GAMMA;
} parsed_t;

/* a struct containing the units used */
typedef struct{
	double t; /* time */
	double m; /* mass */
	double l; /* length */
	double E; /* energy */
	double mstar; /* stars' masses are kept in different units */
} units_t;

/* a struct for the potential, must be malloc'ed */
typedef struct{
	long n; /* number of elements */
	double *r; /* radius */
	double *phi; /* potential */
} potential_t;

/* a struct for the force, must be malloc'ed */
typedef struct{
	long n; /* number of elements */
	double *r; /* radius */
	double *force; /* force */
} force_t;

/* useful structure for central quantities */
typedef struct{
	double rho; /* central mass density */
	double v_rms; /* rms object velocity */
	double rc; /* core radius */
	double m_ave; /* average object mass */
	double n; /* number density of objects */
	double rc_spitzer; /* Spitzer definition of core radius */
	long N_sin; /* number of objects that are single */
	long N_bin; /* number of objects that are binary */
	double n_sin; /* single star number density */
	double n_bin; /* binary star number density */
	double rho_sin; /* central single star mass density */
	double rho_bin; /* central binary star mass density */
	double m_sin_ave; /*average single star mass */
	double m_bin_ave; /* average binary star mass */
	double v_sin_rms; /* rms single star velocity */
	double v_bin_rms; /* rms binary star velocity */
	double w2_ave; /* average of 2*m*v^2 per average mass for all objects */
	double R2_ave; /* average of R^2 for single stars */
	double mR_ave; /* average of m*R for single stars */
	double a_ave; /* average of a for binaries */
	double a2_ave; /* average of a^2 for binaries */
	double ma_ave; /* average of m*a for binaries */
} central_t;

/* useful structure for core quantities */
/*typedef struct{
	double N;
	double kT;
} core_t;*/

/* to store the velocity dispersion profile */
typedef struct{
	long n;
	double *r;
	double *sigma;
} sigma_t;

/* parameters for orbit */
typedef struct{
	double rp;
	double ra;
	double dQdrp;
	double dQdra;
	long kmax;
	long kmin;
	int circular_flag;
} orbit_rs_t;

/* parameters for calc_p_orb function */
typedef struct{
	double E;
	double J;
	long index;
	long kmin;
	long kmax;
	double rp;
	double ra;
} calc_p_orb_params_t;

/* other useful structs */
typedef struct{
	long N_MAX;
	long N_MAX_NEW;
	long N_STAR;
	long N_STAR_NEW;
	long N_BINARY;
} clus_struct_t;

typedef struct{
	double tot; /* total = kinetic + potential + internal + binary binding + central mass energy */
	double New; /* ??? */
	double ini; /* initial total energy */
	double K; /* total kinetic */
	double P; /* total potential */
	double Eint; /* total internal */
	double Eb; /* total binary binding energy */
} Etotal_struct_t;

typedef struct{
	double rh; /* half-mass radius */
} clusdyn_struct_t;

/********************** Function Declarations ************************/
double sqr(double x);
double cub(double x);
void tidally_strip_stars(void);

void remove_star_center(long j);
void print_results(void);
void print_conversion_script(void);
double potential(double r);	       /* get potential using star.phi */
double potential_serial(double r);
double fastpotential(double r, long kmin, long kmax);
long potential_calculate(void);	/* calculate potential at star locations in star.phi */
long potential_calculate_mimic(void); //to mimic the parallel version
#ifdef USE_MPI
long mpi_potential_calculate(void);
long mpi_potential_calculate2(void);
MPI_Comm inv_comm_create();
#endif
void print_small_output();
void print_denprof_snapshot(char* infile);

/* Bharath: Timing Functions */ 
double timeStartSimple();
void timeEndSimple(double timeStart, double *timeAccum);
void timeStart();
void timeEnd(char* fileName, char *funcName, double *tTime);
void timeStart2(double *st);
void timeEnd2(char* fileName, char *funcName, double *st, double *end, double *tot);
void create_timing_files();
/* End */

/* Bharath: Other refactored functions */
void mpiBcastGlobArrays();
void mpiInitGlobArrays();
void set_global_vars1();
void set_global_vars2();
void get_star_data(int argc, char *argv[], gsl_rng *rng);
void calc_sigma_new();
void calc_central_new();
void bin_vars_calculate();
void calc_potential_new();
void calc_potential_new2();
void compute_energy_new();
void set_energy_vars();
void reset_interaction_flags();
void calc_clusdyn_new();
void calc_timestep(gsl_rng *rng);
void energy_conservation1();
void energy_conservation2();
void new_orbits_calculate();
void toy_rejuvenation();
void pre_sort_comm();
void post_sort_comm();
void findIndices( long N, int blkSize, int i, int* begin, int* end );
void findLimits( long N, int blkSize );
int findProcForIndex( int j );
void set_rng_states();
int get_global_idx(int i);
int get_local_idx(int i);
/* End */

/* Bharath: Functions for handling of binaries for parallel version */
void distr_bin_data();
void collect_bin_data();
void alloc_bin_buf();
/* End */

/* Function to handle I/O in parallel version */
void cat_and_rm_files(char* file_ext);
void mpi_merge_files();
void save_root_files();
void save_root_files_helper(char* file_ext);
void rm_files();
void rm_files_helper(char* file_ext);
/* End */

void comp_mass_percent(void);
void comp_multi_mass_percent(void);
orbit_rs_t calc_orbit_rs(long si, double E, double J);
double get_positions(void);	/* get positions and velocities */
void perturb_stars(double Dt);	/* take a time step (perturb E,J) */
long FindZero_r_serial(long kmin, long kmax, double r);
long FindZero_r(long x1, long x2, double r);
long FindZero_Q(long j, long x1, long x2, double E, double J);
double potentialDifference(int particleIndex);
void ComputeEnergy(void);

#ifdef USE_MPI
void mpi_ComputeEnergy(void);
#endif

void PrintLogOutput(void);
double GetTimeStep(gsl_rng *rng);
long CheckStop(struct tms tmsbuf);
void ComputeIntermediateEnergy(void);
void energy_conservation3(void);
void set_velocities3(void);
void mpi_set_velocities3(void);
double rtbis(double (*func) (double), double x1, double x2, double xacc);
double trapzd(double (*func) (double), double a, double b, long n);
double qsimp(double (*func) (double), double a, double b);

void splint(double xa[], double ya[], double y2a[], long n, double x, double *y);
void spline(double x[], double y[], long n, double yp1, double ypn, double y2[]);
void print_2Dsnapshot(void);
void get_physical_units(void);
void update_vars(void);

void print_version(FILE *stream);
void cmc_print_usage(FILE *stream, char *argv[]);
void parser(int argc, char *argv[], gsl_rng *r);
void PrintFileOutput(void);
char *sprint_star_dyn(long k, char string[MAX_STRING_LENGTH]);
char *sprint_bin_dyn(long k, char string[MAX_STRING_LENGTH]);

/* Unmodified Numerical Recipes Routines */
void nrerror(char error_text[]);
double *vector(long nl, long nh);
int *ivector(long nl, long nh);
void free_vector(double *v, long nl, long nh);
void free_ivector(int *v, long nl, long nh);

/* fits stuff */
void load_fits_file_data(void);

/* stellar evolution stuff */
void stellar_evolution_init(void);
void do_stellar_evolution(gsl_rng *rng);
void write_stellar_data(void);
void handle_bse_outcome(long k, long kb, double *vs, double tphysf);
void cp_binmemb_to_star(long k, int kbi, long knew);
void cp_SEvars_to_newstar(long oldk, int kbi, long knew);
void cp_m_to_newstar(long oldk, int kbi, long knew);
void cp_SEvars_to_star(long oldk, int kbi, star_t *target_star);
void cp_m_to_star(long oldk, int kbi, star_t *target_star);
void cp_SEvars_to_newbinary(long oldk, int oldkbi, long knew, int kbinew);
void cp_starSEvars_to_binmember(star_t instar, long binindex, int bid);
void cp_starmass_to_binmember(star_t instar, long binindex, int bid);
double r_of_m(double M);
void cmc_bse_comenv(binary_t *tempbinary, double cmc_l_unit, double RbloodySUN, double *zpars, double *vs, int *fb);

/* Fewbody stuff */
void destroy_obj(long i);
void destroy_obj_new(long i);
void destroy_binary(long i);
long create_star(int idx, int dyn_0_se_1);
long create_binary(int idx, int dyn_0_se_1);
void dynamics_apply(double dt, gsl_rng *rng);
void perturb_stars_fewbody(double dt, gsl_rng *rng);
void qsorts_new(void);
void qsorts(star_t *s, long n);
void units_set(void);
void central_calculate(void);

#ifdef USE_MPI
void mpi_central_calculate(void);
void mpi_central_calculate1(void);
void mpi_central_calculate2(void);
int sample_sort_old( star_t        *starData,
                  long          *local_N,
                  MPI_Datatype  eType,
                  MPI_Comm      commgroup,
                  int           num_samples );

typedef star_t type;
typedef double keyType;
void remove_stripped_stars(type* buf, int* local_N);
int sample_sort( 	type			*buf,
						int			*local_N,
						MPI_Datatype dataType,
						MPI_Datatype bDataType,
						MPI_Comm    commgroup,
						int			n_samples );
void load_balance( 	type 				*inbuf,
							type 				*outbuf,
							binary_t			*b_inbuf,
							binary_t			*b_outbuf,
							int 				*expected_count, 
							int				*actual_count, 
							int 				myid, 
							int 				procs, 
							MPI_Datatype 	dataType, 	
							MPI_Datatype 	b_dataType, 	
							MPI_Comm			commgroup	);
#endif

central_t central_hard_binary(double ktmin, central_t old_cent);
void clusdyn_calculate(void);
#ifdef USE_MPI
void mpi_clusdyn_calculate(void);
#endif

void print_interaction_status(char status_text[]);
void print_interaction_error(void);
long star_get_id_new(void);
long star_get_merger_id_new(long id1, long id2);
double calc_n_local(long k, long p, long N_LIMIT);
double calc_Ai_local(long k, long kp, long p, double W, long N_LIMIT);
void calc_encounter_dyns(long k, long kp, double v[4], double vp[4], double w[4], double *W, double *rcm, double vcm[4], gsl_rng *rng, int setY);
void set_star_EJ(long k);
void set_star_news(long k);
void set_star_olds(long k);
void zero_star(long j);
void zero_binary(long j);

void sscollision_do(long k, long kp, double rperi, double w[4], double W, double rcm, double vcm[4], gsl_rng *rng);
void merge_two_stars(star_t *star1, star_t *star2, star_t *merged_star, double *vs, struct rng_t113_state* s);
double coll_CE(double Mrg, double Mint, double Mwd, double Rrg, double vinf);

void print_initial_binaries(void);

void bs_calcunits(fb_obj_t *obj[2], fb_units_t *bs_units);
fb_ret_t binsingle(double *t, long ksin, long kbin, double W, double bmax, fb_hier_t *hier, gsl_rng *rng);

void bb_calcunits(fb_obj_t *obj[2], fb_units_t *bb_units);
fb_ret_t binbin(double *t, long k, long kp, double W, double bmax, fb_hier_t *hier, gsl_rng *rng);

double binint_get_mass(long k, long kp, long id);
long binint_get_startype(long k, long kp, long id);
long binint_get_indices(long k, long kp, long id, int *bi);
void binint_log_obj(fb_obj_t *obj, fb_units_t units);
void binint_log_status(fb_ret_t retval);
void binint_log_collision(const char interaction_type[], long id, double mass, double r, fb_obj_t obj, long k, long kp, long startype);
void binint_do(long k, long kp, double rperi, double w[4], double W, double rcm, double vcm[4], gsl_rng *rng);

double simul_relax(gsl_rng *rng);
double simul_relax_new(void);
#ifdef USE_MPI
double mpi_simul_relax_new(void);
#endif

void break_wide_binaries(void);
void calc_sigma_r(void);
void mpi_calc_sigma_r(void);
void zero_out_array(double* ptr, int size);

double sigma_r(double r);

int remove_old_star(double time, long k);

/* signal/GSL error handling stuff */
void toggle_debugging(int signal);
void exit_cleanly(int signal);
void sf_gsl_errhandler(const char *reason, const char *file, int line, int gsl_errno);
void close_buffers(void);
void trap_sigs(void);
void free_arrays(void);

/* if we are using gcc, we can remove the annoying warning */
#ifdef __GNUC__
static double SQR(double a) __attribute__ ((always_inline,pure));
static inline double SQR(double a){return a*a;}
#else
static double sqrarg;
#define SQR(a) ((sqrarg=(a)) == 0.0 ? 0.0 : sqrarg*sqrarg)
#endif

/* black hole accretion stuff (loss cone) */
void get_3d_velocities(double *w, double vr, double vt);
void do_random_step(double *w, double beta, double delta);
double check_angle_w_w_new(double *w, double *w_new, double delta);
double calc_P_orb(long index);
double calc_p_orb_f(double x, void *params);
double calc_p_orb_f2(double x, void *params);
double calc_p_orb_gc(double x, void *params);
void bh_rand_walk(long index, double v[4], double vcm[4], double beta, double dt);

/* potential calculation speed-up*/
long check_if_r_around_last_index(long last_index, double r);

struct Search_Grid {
   /* user modifiable parameters */
   /* starsPerBin is the estimated (anticipated) number of stars per search
    * bin. It is only used to calculate interpol_coeff and grid length.
    */
   long starsPerBin;
   long max_length;
   long min_length;
   /* fraction determines the interpol_coeff such that 
    * grid.radius[fraction*grid->length] is approx. 
    * star[clus.N_MAX*fraction].r. Depending how well the power_law_exponent 
    * fits the distribution of star[i::starsPerBin].r the better this is 
    * fulfilled*/
   double fraction;
   /* Only particle_fraction * clus.N_MAX stars should be used to construct the
    * grid (i.e. we ignore a possible outer halo).*/
   double particle_fraction;
   double power_law_exponent;
   /* You should not change any of the following variables yourself 
    * unless, of course, you know what you are doing.
    */
   long *radius;
   long length;
   double interpol_coeff;
};

struct Interval {
  long min;
  long max;
};

struct Search_Grid *
search_grid_initialize(double power_law_exponent, double fraction, long starsPerBin, double part_frac);

double search_grid_estimate_prop_const(struct Search_Grid *grid);
/* This function does not belong to the public API! */
void search_grid_allocate(struct Search_Grid *grid);
void search_grid_update(struct Search_Grid *grid);

struct Interval
search_grid_get_interval(struct Search_Grid *grid, double r);

long search_grid_get_grid_index(struct Search_Grid *grid, double r);
double search_grid_get_r(struct Search_Grid *grid, long index);
double search_grid_get_grid_indexf(struct Search_Grid *grid, double r);
void search_grid_free(struct Search_Grid *grid);
void search_grid_print_binsizes(struct Search_Grid *grid);

#ifdef DEBUGGING
#include <glib.h>
void load_id_table(GHashTable* ids, char *filename);
#endif
struct star_coords get_position(long index, double E, double J, double old_r, orbit_rs_t orbit);
orbit_rs_t calc_orbit_new_J(long index, double J, struct star_coords old_pos, orbit_rs_t orbit_old);
void set_a_b(long index, long k, double *a, double *b);
long find_zero_Q_slope(long index, long k, double E, double J, int positive);
//long get_positive_Q_index(long index, double E, double J);
long find_zero_Q(long j, long kmin, long kmax, long double E, long double J);
//extern inline long double function_q(long j, long double r, long double pot, long double E, long double J);
orbit_rs_t calc_orbit_new(long index, double E, double J);
double calc_average_mass_sqr(long index, long N_LIMIT);
double floateq(double a, double b);
double sigma_tc_nd(double n, double m1, double r1, double m2, double vinf);
double sigma_tc_nn(double na, double ma, double ra, double nb, double mb, double rb, double vinf);
double Tl(int order, double polytropicindex, double eta);
double Etide(double rperi, double Mosc, double Rosc, double nosc, double Mpert);
struct Interval get_r_interval(double r);
double calc_vr_within_interval(double r, void *p);
double calc_vr_in_interval(double r, long index, long k, double E, double J);
double calc_vr(double r, long index, double E, double J);
double find_root_vr(long index, long k, double E, double J);
double calc_pot_in_interval(double r, long k);
void remove_star(long j, double phi_rtidal, double phi_zero);
inline double function_q(long j, long double r, long double pot, long double E, long double J);
void vt_add_kick(double *vt, double vs1, double vs2, struct rng_t113_state* rng_st);

void parse_snapshot_windows(char *option_string);
void print_snapshot_windows(void);
int valid_snapshot_window_units(void);
void write_snapshot(char *filename);

#include "cmc_bse_utils.h"

/* macros */ 
#define Q_function(j, r, pot, E, J) (2.0 * ((E) - ((pot) + PHI_S((r), j))) - SQR((J) / (r)) )
/* correction to potential due to subtracting star's contribution, and adding self-gravity */
/* #define PHI_S(rad, j) ( ((rad)>=star[(j)].r ? star[(j)].m/(rad) : star[(j)].m/star[(j)].r) * (1.0/clus.N_STAR) - 0.5*star[(j)].m/clus.N_STAR/(rad) ) */
/* correction to potential due to subtracting star's contribution (this is what Marc uses) */
#define PHI_S(rad, j) ( ((rad)>=star[(j)].r ? star[(j)].m/(rad) : star[(j)].m/star[(j)].r) * (1.0/clus.N_STAR) )
#define MPI_PHI_S(rad, j) ( ((rad)>=star_r[(j)] ? star_m[(j)]/(rad) : star_m[(j)]/star_r[(j)]) * (1.0/clus.N_STAR) )

#ifdef USE_MPI
#define function_Q(j, k, E, J) (2.0 * ((E) - (star_phi[(k)] + MPI_PHI_S(star_r[(k)], j))) - SQR((J) / star_r[(k)]))
#else
#define function_Q(j, k, E, J) (2.0 * ((E) - (star[(k)].phi + PHI_S(star[(k)].r, j))) - SQR((J) / star[(k)].r))
#endif

#define gprintf(args...) if (!quiet) {fprintf(stdout, args);}

#ifdef USE_MPI
#define diaprintf(args...) if (!quiet) { if(myid == 0) { fprintf(stdout, "DIAGNOSTIC: %s(): ", __FUNCTION__); fprintf(stdout, args); }}
#else
#define diaprintf(args...) if (!quiet) {fprintf(stdout, "DIAGNOSTIC: %s(): ", __FUNCTION__); fprintf(stdout, args);}
#endif

#define dprintf(args...) if (debug) {fprintf(stderr, "DEBUG: in proc %d, %s(): ", myid, __FUNCTION__); fprintf(stderr, args);}

#define dmpiprintf(args...) if (USE_MPI) { fprintf(stderr, "DEBUG: %s(): ", __FUNCTION__); fprintf(stderr, args); }
#define rootprintf(args...) if (debug) {if(myid == 0) { fprintf(stderr, "DEBUG: %s(): ", __FUNCTION__); fprintf(stderr, args); }}

#ifdef USE_MPI
#define wprintf(args...) { if(myid==0) { fprintf(stderr, "WARNING: %s(): ", __FUNCTION__); fprintf(stderr, args);}}
#else
#define wprintf(args...) { fprintf(stderr, "WARNING: %s(): ", __FUNCTION__); fprintf(stderr, args);}
#endif

#define eprintf(args...) {fprintf(stderr, "ERROR: %s:%d in %s(): ", __FILE__, __LINE__, __FUNCTION__); fprintf(stderr, args);}
#ifdef DEBUGGING
#undef MAX
#undef MIN
#endif
#define MAX(a, b) ((a)>=(b)?(a):(b))
#define MIN(a, b) ((a)<=(b)?(a):(b))
