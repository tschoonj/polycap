//Polycap program, based on Laszlo Vincze's code.
//Reworked for more clear overview of simulation + simulation of confocally arranged optics
//Date of birth: 20171102

// ---------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------
#include "config.h"
#include <stdio.h>
#ifdef _WIN32
  #define _CRT_RAND_S // for rand_s -> see https://msdn.microsoft.com/en-us/library/sxtz2fa8.aspx
#endif
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
#include <omp.h> /* openmp header */
#include <limits.h>
#include <xraylib.h>
#ifdef HAVE_EASYRNG
  #include <easy_rng.h>
  #include <easy_randist.h>
  typedef easy_rng_type polycap_rng_type;
  typedef easy_rng polycap_rng;
  #define polycap_rng_alloc(T) easy_rng_alloc(T)
  #define polycap_rng_free(rng) easy_rng_free(rng)
  #define polycap_rng_set(rng, seed) easy_rng_set(rng, seed)
  #define polycap_rng_uniform(rng) easy_rng_uniform(rng)
  #define polycap_rng_mt19937 easy_rng_mt19937
#else
  #include <gsl/gsl_rng.h>
  #include <gsl/gsl_randist.h>
  typedef gsl_rng_type polycap_rng_type;
  typedef gsl_rng polycap_rng;
  #define polycap_rng_alloc(T) gsl_rng_alloc(T)
  #define polycap_rng_free(rng) gsl_rng_free(rng)
  #define polycap_rng_set(rng, seed) gsl_rng_set(rng, seed)
  #define polycap_rng_uniform(rng) gsl_rng_uniform(rng)
  #define polycap_rng_mt19937 gsl_rng_mt19937
#endif
#include <complex.h> //complex numbers required for Fresnel equation (reflect)

#include <gsl/gsl_multifit.h>
#include <stdbool.h>

#define NELEM 92  /* The maximum number of elements possible  */
#define IDIM 1000 /* The maximum number of capillary segments */
#define NDIM 420  /* The number of scattering factors per element */
#define NSPOT 1000  /* The number of bins in the grid for the spot*/
#define IMSIZE 500001
//#define CALFA 4.15189e-4   /* E = [KEV] ! */
//#define CBETA 9.86643e-9   /* E = [KEV] ! */
//#define C 299792458//light speed [m/s]
#define HC 1.23984193E-7 //h*c [keV*cm]
#define N_AVOG 6.022098e+23 //Avogadro constant
#define R0 2.8179403227e-13 //classical electron radius [cm]
#define DELTA 1.e-10
#define EPSILON 1.0e-30

// ---------------------------------------------------------------------------------------------------
// Define structures
struct inp_file
  {
  double sig_rough;
  double sig_wave;
  double corr_length;
  double d_source;
  double d_screen;
  double src_x;
  double src_y;
  double src_sigx;
  double src_sigy;
  double src_shiftx;
  double src_shifty;
  int nelem;
  int iz[NELEM];
  double wi[NELEM];
  double density;
  double e_start;
  double e_final;
  double delta_e;
  int ndet;
  int shape;
  char prf[80];
  char axs[80];
  char ext[80];
  double length; //in cm
  double rad_ext[2]; //PC external radius, in cm
  double rad_int[2]; //single capillary radius, in cm
  double focal_dist[2]; //focal distance at both sides of PC, in cm
  double n_chan;
  char out[80];
  };
//cap.shape, length, rad_ext[2], rad_int[2], focal_dist[2]

struct cap_prof_arrays
  {
  double zarr;
  double profil;
  double sx;
  double sy;
  double d_arr;
  };

struct cap_profile
  {
  int nmax; /*nr of points defined along capillary profile*/
  double rtot1; /*radius at start position*/
  double rtot2; /*radius at end position*/
  double cl;	/*capillary length*/
  double binsize; /*20.e-4 cm*/
  struct cap_prof_arrays *arr; /* will get proper size allocated to it later */
  };

struct amu_cnt
  {
  double amu;
  double cnt;
  double scatf;
  };

struct mumc
  {
  int n_energy;
  struct amu_cnt *arr; /* Actual size defined later (n_energy+1)*double */
  };

struct leakstruct
  {
  double spot[NSPOT][NSPOT], lspot[NSPOT][NSPOT];
  double *leak;
  };

struct ini_polycap
  {
  double eta, n_chan_max; /* estimated open area, n_chan*/
  double cap_unita[2]; /* 2*chan_rad, 0 */
  double cap_unitb[2]; /* 2*chan_rad*cos(60), 2*chan_rad*sin(60) */
  };

struct image_struct
  {
  double xsou, ysou, xsou1, ysou1, wsou;
  double xm, ym, xm1, ym1, warr;
  };

struct calcstruct
  {
  double *sx;
  double *sy;
  polycap_rng *rn;
  double *cnt;
  double *absorb;
  int64_t i_refl;
  int64_t istart;
  int64_t ienter;
  double rh[3];
  double v[3];
  double traj_length;
  double phase;
  double amplitude;
  double *w;
  int iesc;
  int ix;
  };

// ---------------------------------------------------------------------------------------------------
bool polynomialfit(int obs, int degree, 
		   double *dx, double *dy, double *store) /* n, p */
{
  gsl_multifit_linear_workspace *ws;
  gsl_matrix *cov, *X;
  gsl_vector *y, *c;
  double chisq;
 
  int i, j;
 
  X = gsl_matrix_alloc(obs, degree);
  y = gsl_vector_alloc(obs);
  c = gsl_vector_alloc(degree);
  cov = gsl_matrix_alloc(degree, degree);
 
  for(i=0; i < obs; i++) {
    for(j=0; j < degree; j++) {
      gsl_matrix_set(X, i, j, pow(dx[i], j));
    }
    gsl_vector_set(y, i, dy[i]);
  }
 
  ws = gsl_multifit_linear_alloc(obs, degree);
  gsl_multifit_linear(X, y, c, cov, &chisq, ws);
 
  /* store result ... */
  for(i=0; i < degree; i++)
  {
    store[i] = gsl_vector_get(c, i);
  }
 
  gsl_multifit_linear_free(ws);
  gsl_matrix_free(X);
  gsl_matrix_free(cov);
  gsl_vector_free(y);
  gsl_vector_free(c);
  return true; /* we do not "analyse" the result (cov matrix mainly)
		  to know if the fit is "good" */
}

// ---------------------------------------------------------------------------------------------------
// Read in input file
struct inp_file read_cap_data(char *filename)
	{
	FILE *fptr;
	int i;
	struct inp_file cap;

	fptr = fopen(filename,"r");
	if(fptr == NULL){
		printf("%s file does not exist!\n",filename);
		exit(0);
		}
	fscanf(fptr,"%lf",&cap.sig_rough);
	fscanf(fptr,"%lf %lf",&cap.sig_wave, &cap.corr_length); //currently dummies
	fscanf(fptr,"%lf",&cap.d_source);
	fscanf(fptr,"%lf",&cap.d_screen);
	fscanf(fptr,"%lf %lf",&cap.src_x, &cap.src_y);
	fscanf(fptr,"%lf %lf",&cap.src_sigx, &cap.src_sigy);
	fscanf(fptr,"%lf %lf",&cap.src_shiftx, &cap.src_shifty);
	fscanf(fptr,"%d",&cap.nelem);
	for(i=0; i<cap.nelem; i++){
		fscanf(fptr,"%d %lf",&cap.iz[i],&cap.wi[i]);
		cap.wi[i] /= 100.0;
		}
	fscanf(fptr,"%lf",&cap.density);
	fscanf(fptr,"%lf %lf %lf",&cap.e_start,&cap.e_final,&cap.delta_e);
	fscanf(fptr,"%d",&cap.ndet);
	fscanf(fptr,"%d",&cap.shape);
	if(cap.shape == 0 || cap.shape == 1 || cap.shape == 2){
		fscanf(fptr,"%lf %lf %lf %lf %lf %lf %lf",&cap.length,&cap.rad_ext[0],&cap.rad_ext[1],&cap.rad_int[0],&cap.rad_int[1],&cap.focal_dist[0],&cap.focal_dist[1]);
	} else { //additional files to describe (poly)capillary profile were supplied
		fscanf(fptr,"%s",cap.prf);
		fscanf(fptr,"%s",cap.axs);
		fscanf(fptr,"%s",cap.ext);
	}
	fscanf(fptr,"%lf",&cap.n_chan);
	fscanf(fptr,"%s",cap.out);
	fclose(fptr);

	return cap;
	}
// ---------------------------------------------------------------------------------------------------
// Read in polycapillary profile data
struct cap_profile *read_cap_profile(struct inp_file *cap)
	{
	FILE *fptr;
	int i, n_tmp;

	//single capillary profile
	fptr = fopen(cap->prf,"r");
	if(fptr == NULL){
		printf("%s file does not exist.\n",cap->prf);
		exit(0);
		}
	fscanf(fptr,"%d",&n_tmp);

	struct cap_profile *profile = malloc(sizeof(struct cap_profile));
	if(profile == NULL){
		printf("Could not allocate profile memory.\n");
		exit(0);
		}
	profile->arr = malloc(sizeof(struct cap_prof_arrays)*(n_tmp+1));
	if(profile->arr == NULL){
		printf("Could not allocate profile->arr memory.\n");
		exit(0);
		}
	profile->nmax = n_tmp;
	//Continue reading profile data
	for(i=0; i<=profile->nmax; i++){
		fscanf(fptr,"%lf %lf",&profile->arr[i].zarr,&profile->arr[i].profil);
		}
	fclose(fptr);
	//polycapillary central axis
	fptr = fopen(cap->axs,"r");
	if(fptr == NULL){
		printf("%s file does not exist.\n",cap->axs);
		exit(0);
		}
	fscanf(fptr,"%d",&n_tmp);
	if(profile->nmax != n_tmp){
		printf("Inconsistent *.axs file: number of intervals different.\n");
		exit(0);
		}
	for(i=0; i<=profile->nmax; i++){
		fscanf(fptr,"%lf %lf %lf",&profile->arr[i].zarr,&profile->arr[i].sx,&profile->arr[i].sy);
		}
	fclose(fptr);

	//polycapillary external shape
	fptr = fopen(cap->ext,"r");
	if(fptr == NULL){
		printf("%s file does not exist.\n",cap->ext);
		exit(0);
		}
	fscanf(fptr,"%d",&n_tmp);
	if(profile->nmax != n_tmp){
		printf("Inconsistent *.ext file: number of intervals different.\n");
		exit(0);
		}
	for(i=0; i<=profile->nmax; i++){
		fscanf(fptr,"%lf %lf",&profile->arr[i].zarr,&profile->arr[i].d_arr);
		}
	fclose(fptr);

	profile->rtot1 = profile->arr[0].d_arr;
	profile->rtot2 = profile->arr[profile->nmax].d_arr;
	profile->cl = profile->arr[profile->nmax].zarr;
	cap->d_screen = cap->d_screen + cap->d_source + profile->cl; //position of screen on z axis
	profile->binsize = 20.e-4; 

	return profile;
	}
// ---------------------------------------------------------------------------------------------------
// Calculate total cross sections and scatter factor
struct mumc *ini_mumc(struct inp_file *cap)
	{
	int i, j, n_energy_tmp;//,nn=1;
	double e, totmu;
	double scatf;

	n_energy_tmp = (int)( (cap->e_final - cap->e_start) / cap->delta_e);
	struct mumc *absmu = malloc(sizeof(struct mumc));
	if(absmu == NULL){
		printf("Could not allocate absmu memory.\n");
		exit(0);
		}

	absmu->arr = malloc(sizeof(struct amu_cnt)*(n_energy_tmp+1));
	if(absmu->arr == NULL){
		printf("Could not allocate absmu->arr memory.\n");
		exit(0);
		}

	absmu->n_energy = n_energy_tmp;
	for(i=0; i<=absmu->n_energy; i++){
		e = cap->e_start + i*cap->delta_e;
		totmu = 0.;
		scatf = 0.;
		for(j=0; j<cap->nelem;j++){
			totmu = totmu + CS_Total(cap->iz[j],e) * cap->wi[j];

			scatf = scatf + (cap->iz[j] + Fi(cap->iz[j],e) ) * (cap->wi[j] / AtomicWeight(cap->iz[j]) );
			}
		absmu->arr[i].amu = totmu * cap->density;

		absmu->arr[i].scatf = scatf;
		}

	return absmu;
	}
// ---------------------------------------------------------------------------------------------------
struct leakstruct *reset_leak(struct cap_profile *profile,struct mumc *absmu)
	{
	int i, j;
	struct leakstruct *leaks=malloc(sizeof(struct leakstruct));
	if(leaks == NULL){
		printf("Could not allocate leaks memory.\n");
		exit(0);
		}
	leaks->leak = malloc(sizeof(leaks->leak)*(profile->nmax+1));
	if(leaks->leak == NULL){
		printf("Could not allocate leaks->leak memory.\n");
		exit(0);
		}

	for(i=0; i<=profile->nmax; i++) leaks->leak[i] = 0.;
	for(i=0; i<NSPOT; i++){
		for(j=0; j<NSPOT; j++){
			leaks->spot[i][j] = 0.;
			leaks->lspot[i][j] = 0.;
			}
		}
	for(i=0; i<=absmu->n_energy; i++) absmu->arr[i].cnt = 0.;

	return leaks;
	}
// ---------------------------------------------------------------------------------------------------
struct ini_polycap ini_polycap(struct inp_file *cap, struct cap_profile *profile)
	{
	double chan_rad, s_unit;
	struct ini_polycap pcap_ini;

	chan_rad = profile->arr[0].profil; //entrance radius of single capillary
	pcap_ini.eta = chan_rad / profile->rtot1; //devide by external PC entrance radius
	pcap_ini.eta = pcap_ini.eta * pcap_ini.eta * cap->n_chan; //polycapillary open area (assuming circle)
	pcap_ini.n_chan_max = sqrt(12. * cap->n_chan - 3.)/6.-0.5; //amount of 'shells' in hexagon PC
	if(pcap_ini.n_chan_max <= 0){
		printf("N_CHANNEL must be >=7\n");
		exit(0);
		}
	s_unit = profile->rtot1/(pcap_ini.n_chan_max); //width of a single shell
	pcap_ini.cap_unita[0] = s_unit;
	pcap_ini.cap_unita[1] = 0.;
	pcap_ini.cap_unitb[0] = s_unit * cos(M_PI/3.);
	pcap_ini.cap_unitb[1] = s_unit * sin(M_PI/3.);

	return pcap_ini;
	}
// ---------------------------------------------------------------------------------------------------
void norm(double *vect, int ndim)
	{
	int i;
	double sum=0;

	for(i=0;i<ndim;i++) sum = sum+ (*(vect+i)) * (*(vect+i));
	sum = sqrt(sum);

	for(i=0;i<ndim;i++) *(vect+i) /= sum;

	return;
	}
// ---------------------------------------------------------------------------------------------------
double scalar(double vect0[3],double vect1[3])
	{
	int i;
	double sum=0;

	for(i=0; i<3; i++) sum = sum + vect0[i]*vect1[i];

	return sum;
	}
// ---------------------------------------------------------------------------------------------------
// calculates the intersection point coordinates of the photon trajectory and a given linear segment of the capillary wall
int segment(double s0[3], double s1[3], double rad0, double rad1, double rh1[3], double v[3], double rn[3], double *calf)
	{
	int iesc_local = 0;
	double drs[3], ds[3]; //coordinates of previous photon interaction and current point capillary axis, with previous point capillary axis set as origin [0,0,0]
	double vds; //cosine of angle between vectors v and ds (photon propagation and capillary wall segment)
	double a, b;
	double aa[3], bb[3];
	double a0, b0, c0;
	double disc, ck1, ck2; //discriminant (disc) and solutions (ck1 and ck2) of quadratic equation
	double ck; //final solution to the quadratic equation
	double cc; //distance traveled by photon until next interaction
	double s[3], u[3]; //coordinates of capillary axis at interaction distance (s) and normalized interaction coordinates (u)
	double au, ads; //distance between capillary axis and interaction point (au), distance between s0 and s1
	double tga, sga, cga; //tan(gamma), sin(ga) and cos(ga) where gamma is angle between capillary wall and axis
	double gam; //actual angle gamma as in line above
	//rn = capillary surface normal at interaction point

	ck = -1000;

	drs[0] = rh1[0] - s0[0];
	drs[1] = rh1[1] - s0[1];
	drs[2] = rh1[2] - s0[2];

	ds[0] = s1[0] - s0[0];
	ds[1] = s1[1] - s0[1];
	ds[2] = s1[2] - s0[2];
	vds = scalar(v,ds); //cos(angle)/(|v|*|ds|)
	if(fabs(vds) < EPSILON){
		iesc_local = -2;
		return iesc_local;
		//continues in for loop of 'capil', i.e. selects new section of capillary to compare to)
		}

	a = -1*scalar(drs,ds)/vds;
	b = scalar(ds,ds)/vds;

	aa[0] = rh1[0] + a*v[0] - s0[0];
	aa[1] = rh1[1] + a*v[1] - s0[1];
	aa[2] = rh1[2] + a*v[2] - s0[2];

	bb[0] = b*v[0] - s1[0] + s0[0];
	bb[1] = b*v[1] - s1[1] + s0[1];
	bb[2] = b*v[2] - s1[2] + s0[2];

	a0 = scalar(bb,bb)-(rad1-rad0)*(rad1-rad0);
	b0 = (double)2.*(scalar(aa,bb)-rad0*(rad1-rad0));
	c0 = scalar(aa,aa) - rad0*rad0;

	if(fabs(a0) <= EPSILON){ //equation actually more like y = bx + c
		ck1 = -c0/b0;
		ck2 = -1000;
		}
		else
		{ //actual quadratic equation
		disc = b0*b0 - 4.*a0*c0;
		if(disc < (double)0.){
			iesc_local = -2;
			return iesc_local;
			}
		disc = sqrt(disc);
		ck1 = (-b0+disc)/(2.*a0);
		ck2 = (-b0-disc)/(2.*a0);
		}
	if(ck1 > (double)EPSILON && ck1 <= (double)1.) ck=ck1;
	if(ck2 > (double)EPSILON && ck2 <= (double)1.) ck=ck2;
	if(ck == -1000){ //this is true when both ifs above are false
		iesc_local = -2;
		return iesc_local;
		}

	cc = a + ck*b;
	if(cc < 1.e-10){
		iesc_local = -2;
		return iesc_local;
		}

	//location of next intersection point
	rh1[0] = rh1[0] + cc*v[0];
	rh1[1] = rh1[1] + cc*v[1];
	rh1[2] = rh1[2] + cc*v[2];

	s[0] = s0[0] + ck*ds[0]; //new point along capillary axis at intersection distance
	s[1] = s0[1] + ck*ds[1];
	s[2] = s0[2] + ck*ds[2];

	u[0] = rh1[0] - s[0]; //normalized coordinates of intersection point compared to axis
	u[1] = rh1[1] - s[1];
	u[2] = rh1[2] - s[2];

	//surface normal at the new intersection point
	au = sqrt(scalar(u,u));
	ads = sqrt(scalar(ds,ds));

	tga = (rad0 - rad1)/ads; //note: this will be negative if rad0 < rad1 (confocal geometry)
	gam = atan(tga);
	cga = cos(gam);
	sga = sin(gam);

	rn[0] = cga*u[0]/au + sga*ds[0]/ads;
	rn[1] = cga*u[1]/au + sga*ds[1]/ads;
	rn[2] = cga*u[2]/au + sga*ds[2]/ads;
	norm(rn, 3);

	*calf = scalar(rn,v); //cos of angle between rn (surface normal) and v (photon direction)
	if(*calf < 0.0){
		iesc_local = -2;
		return iesc_local;
		}

	iesc_local = 0;
	return iesc_local;
	}
// ---------------------------------------------------------------------------------------------------
int reflect(double alf, struct inp_file *cap, struct mumc *absmu, struct cap_profile *profile, struct leakstruct *leaks, struct calcstruct *calc)
	{
	int i;
	double desc; //distance in capillary at which photon escaped divided by propagation vector in z direction
	double e; //energy
	double cons1, r_rough;
	double complex alfa, beta; //alfa and beta component for Fresnel equation delta term (delta = alfa - i*beta)
	double complex rtot; //reflectivity
	double wleak;
	double c; //distance between photon interaction and screen, divided by propagation vector in z direction
	double xp, yp; //position on screen where photon will end up if unobstructed
	int ind_x, ind_y; //indices of array lspot where photon will hit screen

	//escape
	desc = (profile->cl + cap->d_source - calc->rh[2]) / calc->v[2];
	if(desc < 0) desc = profile->cl;
	for(i=0; i <= absmu->n_energy; i++){
		e = cap->e_start + i * cap->delta_e;
		cons1 = (double)(1.01358e0*e)*alf*cap->sig_rough;
		r_rough = exp(-1*cons1*cons1);

		alfa = (double)(HC/e)*(HC/e)*((N_AVOG*R0*cap->density)/(2*M_PI)) * absmu->arr[i].scatf;
		beta = (double) (HC)/(4.*M_PI) * (absmu->arr[i].amu/e);

		//reflectivity according to Fresnel expression
		rtot = ((complex double)alf - csqrt(cpow((complex double)alf,2) - 2.*(alfa - beta*I))) / ((complex double)alf + csqrt(cpow((complex double)alf,2) - 2.*(alfa - beta*I)));
		rtot = creal(cpow(cabs(rtot),2.));
		//printf("Energy: %f, creal(rtot): %lf, cimag(rtot): %lf\n",e, creal(rtot), cimag(rtot));
		wleak = (1.-rtot) * calc->w[i] * exp(-1.*desc * absmu->arr[i].amu);
		leaks->leak[i] = leaks->leak[i] + wleak;
		if(i==0){
			c = (cap->d_screen - calc->rh[2]) / calc->v[2];
			xp = calc->rh[0] + c*calc->v[0];
			yp = calc->rh[1] + c*calc->v[1];
			ind_x = (int)floor(xp/profile->binsize)+NSPOT/2;
			ind_y = (int)floor(yp/profile->binsize)+NSPOT/2;
			if(ind_x < NSPOT && ind_x >= 0){
				if(ind_y < NSPOT && ind_y >= 0){
					leaks->lspot[ind_x][ind_y] = leaks->lspot[ind_x][ind_y] + wleak;
					}
				}
			}
		calc->w[i] = calc->w[i] * rtot * r_rough;
		} //for(i=0; i <= absmu->n_energy; i++)

	if(calc->w[0] < 1.e-4) return -2;

	return 0;
	}
// ---------------------------------------------------------------------------------------------------
void start(struct mumc *absmu, struct cap_profile *profile, struct ini_polycap *pcap_ini, struct inp_file *cap, int icount, struct image_struct *imstr, struct calcstruct *calc)
	{
	int i, flag_restart;
	int ix_cap, iy_cap; //indices of selected channel
	double dx; //distance between photon's source origin and PC entrance coordinates (projected on same plane)
		// dx is just a measure to see if can quit while loop or not, essentially only runs once through it
	double r; //random nr
	double ra, rb, rr; //coordinates (ra, rb) and distance from center (rr) of selected channel
	double cosphi, sinphi; //angle between horizontal and selected channel (along rr)
	double cx; //relative distance from PC centre (compared to PC external radius)
	double rad; //random radius from centre of source size (between 0 and sigx)
	double fi; //random angle in which photon was emitted from source (between 0 and 2M_PI) 
	double x, y; //coordinates from which photon was emitted
	double xpc, ypc; //coordinates from which photon was emitted given src_sigx and src_sigy are (nearly) 0
	double gamma, w_gamma; //photon origin to selected capillary angle and weight (cos(gamma))
	double c; //distance bridged by photon between source and selected capillary

	calc->i_refl = 0;

	for(i=0; i <= absmu->n_energy; i++)
		calc->w[i] = 1.0;
	dx = 2e9; //set dx very high so it is certainly > single capillary radius (profil)
	while(dx > profile->arr[0].profil){
		//select capil
		flag_restart = 0;
		do{
			r = polycap_rng_uniform(calc->rn);
			ix_cap = floor( pcap_ini->n_chan_max * (2.*fabs(r)-1.) + 0.5);
			r = polycap_rng_uniform(calc->rn);
			iy_cap = floor( pcap_ini->n_chan_max * (2.*fabs(r)-1.) + 0.5);
			} while(abs(iy_cap+ix_cap) > pcap_ini->n_chan_max );
		//calc_tube/calc_axs
		ra = ix_cap*pcap_ini->cap_unita[0] + iy_cap*pcap_ini->cap_unitb[0];
		rb = ix_cap*pcap_ini->cap_unita[1] + iy_cap*pcap_ini->cap_unitb[1];
		rr = sqrt(ra*ra+rb*rb);
		if(rr <= DELTA){
			cosphi = 0;
			sinphi = 0;
			}else{
			cosphi = ra/rr;
			sinphi = rb/rr;
			}
		cx = rr / profile->rtot1;
		for(i=0; i <= profile->nmax; i++){
			calc->sx[i] = profile->arr[i].d_arr * cosphi * cx;
			calc->sy[i] = profile->arr[i].d_arr * sinphi * cx;
			}

		//sourcp
		r = polycap_rng_uniform(calc->rn);
		rad = cap->src_x * sqrt(fabs(r)); //sqrt to simulate source intensity distribution (originally probably src_x * r/sqrt(r) )
		r = polycap_rng_uniform(calc->rn);
		fi = 2.0*M_PI*fabs(r);
		x = rad * cos(fi) + cap->src_shiftx;
		y = rad * sin(fi) + cap->src_shifty;
		calc->rh[0] = x;
		calc->rh[1] = y;
		calc->rh[2] = 0.0;
		if(cap->src_sigx*cap->src_sigy < 1.e-20){ //uniform distribution over PC entrance
			r = polycap_rng_uniform(calc->rn);
			rad = profile->arr[0].profil * sqrt(fabs(r));
			r = polycap_rng_uniform(calc->rn);
			fi = 2.*M_PI*fabs(r);
			xpc = rad * cos(fi) + ra;
			ypc = rad * sin(fi) + rb;
			calc->v[0] = xpc - x;
			calc->v[1] = ypc - y;
			calc->v[2] = cap->d_source;
			} else { //non-uniform distribution
			r = polycap_rng_uniform(calc->rn);
			calc->v[0] = cap->src_sigx * (1.-2.*fabs(r));
			r = polycap_rng_uniform(calc->rn);
			calc->v[1] = cap->src_sigy * (1.-2.*fabs(r));
			calc->v[2] = 1.;
			}
		norm(calc->v, 3); //normalize vector v
		calc->phase = 0.;
		calc->amplitude = 1.;
		calc->traj_length = 0.;

		gamma = sqrt( (ra-calc->rh[0])*(ra-calc->rh[0]) + 
			(rb-calc->rh[0])*(rb-calc->rh[0]) ) / cap->d_source;
		gamma = atan(gamma);
		w_gamma = cos(gamma); /* weight factor to take into account the effective solid-angle 
					of the capillary channel from the source point, 
					should be nearly 1 for d_source > 10 cm */
		if(icount < IMSIZE-1){
			imstr[icount].xsou = calc->rh[1];
			imstr[icount].ysou = calc->rh[0];
			imstr[icount].xsou1 = calc->v[1];
			imstr[icount].ysou1 = calc->v[0];
			imstr[icount].wsou = 1.0;
			}
		c = ( cap->d_source - calc->rh[2]) / calc->v[2];
		calc->rh[0] = calc->rh[0] + c * calc->v[0];
		calc->rh[1] = calc->rh[1] + c * calc->v[1];
		calc->rh[2] = cap->d_source;
		calc->traj_length = calc->traj_length + c;
			/*first segment is to reach capillary entrance*/

		//rotx -As long as theta_align =0 this doesn't actually change the values of calc->v
		//cap->theta_align not used as in original code was just set to 0
//		v1[0] = calc->v[0]*cos(cap->theta_align) - calc->v[2]*sin(cap->theta_align);
//		v1[1] = calc->v[1];
//		v1[2] = calc->v[0]*sin(cap->theta_align) + calc->v[2]*cos(cap->theta_align);
//		calc->v[0] = v1[0];
//		calc->v[1] = v1[1];
//		calc->v[2] = v1[2];

		calc->iesc = 0;
		calc->istart++; //photon was started for simulation
		dx = sqrt( (calc->rh[0]-ra)*(calc->rh[0]-ra) + 
			(calc->rh[1]-rb)*(calc->rh[1]-rb));
		} /*end of while(dx > profile->arr[0].profil)*/

	calc->ienter++; //photon entered the PC
	for(i=0; i<= absmu->n_energy;i++){
		calc->w[i] = calc->w[i] * w_gamma;
		}

	return;
	}
// ---------------------------------------------------------------------------------------------------
void capil(struct mumc *absmu, struct cap_profile *profile, struct inp_file *cap, struct leakstruct *leaks, struct calcstruct *calc)
	{
	int64_t i;
	double s0[3], s1[3]; //selected capillary axis coordinates
	double rad0, rad1; //capillary radius
	double rh1[3]; //essentially coordinates of photon in capillary at last interaction
	double rn[3],calf; //capillary surface normal at interaction point rn, cos of angle between capillary normal at interaction point and photon direction before interaction
	double alf; //angle between capillary normal at interaction point and photon direction before interaction
	double delta_traj[3]; //relative coordinates of new interaction point compared to previous interaction
	double ds; //distance between interactions
	double w0, w1;
	double salf2; //2* sin(alf) with alf=interaction angle

	calc->iesc = 0;
	if(calc->i_refl == 0) calc->ix = 0;

	//intersection
	for(i=calc->ix+1; i<=profile->nmax; i++){
		s0[0] = calc->sx[i-1];
		s0[1] = calc->sy[i-1];
		s0[2] = profile->arr[i-1].zarr;
		s1[0] = calc->sx[i];
		s1[1] = calc->sy[i];
		s1[2] = profile->arr[i].zarr;
		rad0 = profile->arr[i-1].profil;
		rad1 = profile->arr[i].profil;
		rh1[0] = calc->rh[0];
		rh1[1] = calc->rh[1];
		rh1[2] = calc->rh[2] - cap->d_source;
		calc->iesc = segment(s0,s1,rad0,rad1,rh1,calc->v,rn,&calf);
		if(calc->iesc == 0){
			calc->ix = i-1;
			break; //break out of for loop and store previous i in calc->ix
			}
		}


	if(calc->iesc !=0){
		calc->iesc = 1;
		}
		else //calc->iesc == 0
		{
		delta_traj[0] = rh1[0] - calc->rh[0];
		delta_traj[1] = rh1[1] - calc->rh[1];
		delta_traj[2] = rh1[2] + cap->d_source - calc->rh[2];
		ds = sqrt(scalar(delta_traj,delta_traj));
		calc->traj_length = calc->traj_length + ds;
		//store new interaction coordinates in appropriate array
		calc->rh[0] = rh1[0];
		calc->rh[1] = rh1[1];
		calc->rh[2] = rh1[2] + cap->d_source;

		if(fabs(calf) > 1.0){
			printf("COS(alfa) > 1\n");
			calc->iesc = -1;
			}
			else
			{
			alf = acos(calf);
			alf = M_PI_2 - alf;
			w0 = calc->w[0];

			calc->iesc = reflect(alf,cap,absmu,profile,leaks,calc);

			if(calc->iesc != -2){
				w1 = calc->w[0];
				calc->absorb[calc->ix] = calc->absorb[calc->ix] + w0 - w1;

				salf2 = 2.0*sin(alf);
				calc->v[0] = calc->v[0] - salf2*rn[0];
				calc->v[1] = calc->v[1] - salf2*rn[1];
				calc->v[2] = calc->v[2] - salf2*rn[2];

				norm(calc->v, 3);
				calc->i_refl++; //add a reflection
				}
			}
		}

	return;
	}
// ---------------------------------------------------------------------------------------------------
void count(struct mumc *absmu, struct inp_file *cap, int icount, struct cap_profile *profile, struct leakstruct *leaks, struct image_struct *imstr, struct calcstruct *calc)
	{
	int i;
	double cc; //distance between last interaction and capillary exit, divided by propagation vector in z
	double xpend, ypend; //coordinates of photon at end of capillary if rendered unobstructed
	double v_hex1[2], v_hex2[2], v_hex3[2]; //normal vectors of edges of hexagonal PC shape
	double hex_edge_dist; //distance from PC centre to middle of hex edge (along normal on edge)
	double dp1, dp2, dp3; //length of xpend and ypend projected on hex edge norm
	double c; //distance between last interaction and screen position, divided by propagation vector in z
	double xp, yp; //photon position on screen if rendered unobstructed
	double delta_traj[3]; //photon trajectory from last interaction to screen
	double ds; //distance between last interaction and screen
	int ind_x, ind_y; //indices of spot array corresponding to photon coordinate on screen

	//simulate hexagonal polycapillary housing
	cc = ((cap->d_source+profile->cl)-calc->rh[2])/calc->v[2];
	xpend  = calc->rh[0] + cc*calc->v[0];
	ypend  = calc->rh[1] + cc*calc->v[1];
	v_hex1[0] = 0; //vert hex edges x vector
	v_hex1[1] = 1; //vert hex edges y vector
	v_hex2[0] = cos(M_PI/6); //upper right and lower left edges x vector
	v_hex2[1] = sin(M_PI/6); //upper right and lower left edges y vector
	v_hex3[0] = cos(-1.*M_PI/6); //upper left and lower right edges x vector
	v_hex3[1] = sin(-1.*M_PI/6); //upper left and lower right edges y vector
	hex_edge_dist = sqrt(profile->rtot2 * profile->rtot2 - (profile->rtot2/2)*(profile->rtot2/2));
	//calculate dot products and see if magnitude is > distance from centre to hex side
	dp1 = fabs(v_hex1[0]*xpend + v_hex1[1]*ypend);
	dp2 = fabs(v_hex2[0]*xpend + v_hex2[1]*ypend);
	dp3 = fabs(v_hex3[0]*xpend + v_hex3[1]*ypend);
	if(dp1 > hex_edge_dist || dp2 > hex_edge_dist || dp3 > hex_edge_dist){
		//photon is outside of PC exit area
		calc->iesc = -3;
		}
		else //photon inside PC exit area
		{
		for(i=0; i <= absmu->n_energy; i++){
			calc->cnt[i] = calc->cnt[i] + calc->w[i];
			} //for(i=0; i <= absmu->n_energy; i++)

		c = (cap->d_screen - calc->rh[2]) / calc->v[2];
		xp = calc->rh[0] + c*calc->v[0];
		yp = calc->rh[1] + c*calc->v[1];

		delta_traj[0] = c*calc->v[0];
		delta_traj[1] = c*calc->v[1];
		delta_traj[2] = c*calc->v[2];
		ds = sqrt(scalar(delta_traj,delta_traj));
		calc->traj_length = calc->traj_length + ds;

		ind_x = (int)floor(xp/profile->binsize)+NSPOT/2;
		ind_y = (int)floor(yp/profile->binsize)+NSPOT/2;
		if(ind_x < NSPOT && ind_x >= 0){
			if(ind_y < NSPOT && ind_y >= 0){
				leaks->spot[ind_x][ind_y] = leaks->spot[ind_x][ind_y] + calc->w[0];
				}
			}

		if(icount <= IMSIZE-1){
			imstr[icount].xm = yp;
			imstr[icount].ym = xp;
			imstr[icount].xm1 = calc->v[1];
			imstr[icount].ym1 = calc->v[0];
			imstr[icount].warr = calc->w[0];
			}
		} //if(dp1 > hex_edge_dist || dp2 > hex_edge_dist || dp3 > hex_edge_dist) ... else ...
	return;
	}

// ---------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------

struct calcstruct *init_calcstruct(unsigned long int seed, struct cap_profile *profile, struct mumc *absmu) {
	const polycap_rng_type *T = polycap_rng_mt19937; //Mersenne twister rng
	struct calcstruct *calc = malloc(sizeof(struct calcstruct));
	int j;

	/*give arrays inside calc struct appropriate dimensions*/
	calc->sx = malloc(sizeof(double)*(profile->nmax+1));
	if(calc->sx == NULL){
		printf("Could not allocate calc->sx memory.\n");
		exit(0);
	}
	calc->sy = malloc(sizeof(double)*(profile->nmax+1));
	if(calc->sy == NULL){
		printf("Could not allocate calc->sy memory.\n");
		exit(0);
	}
	calc->absorb = malloc(sizeof(double)*(profile->nmax+1));
	if(calc->absorb == NULL){
		printf("Could not allocate calc->absorb memory.\n");
		exit(0);
	}
	calc->w = malloc(sizeof(double)*(absmu->n_energy+1));
	if(calc->w == NULL){
		printf("Could not allocate calc->w memory.\n");
		exit(0);
	}
	calc->cnt = malloc(sizeof(double)*(absmu->n_energy+1));
	if(calc->cnt == NULL){
		printf("Could not allocate calc->cnt memory.\n");
		exit(0);
	}
	/*copy correct values into corresponding calc struct variable*/
	calc->istart = 0;
	calc->ienter = 0;
	calc->iesc = 0;
	calc->ix = 0;
	for(j=0; j<=profile->nmax; j++){
		calc->sx[j] = profile->arr[j].sx;
		calc->sy[j] = profile->arr[j].sy;
		calc->absorb[j] = 0.;
	}
	for(j=0; j<=absmu->n_energy;j++){
		calc->cnt[j] = 0.;
	}
	//Give each thread unique rng range.
	calc->rn = polycap_rng_alloc(T);
	polycap_rng_set(calc->rn, seed);

	return calc;
}

// ---------------------------------------------------------------------------------------------------
struct cap_profile *def_cap_profile(unsigned long int shape, double length, double rad_ext[2], double rad_int[2], double focal_dist[2]){
	struct cap_profile *profile = malloc(sizeof(struct cap_profile));
	int i,j;
	double pc_x[4], pc_y[4], coeff[3];
	double slope, b, k, a;

	if(shape == 0 || shape == 1 || shape ==2){
		//Make profile array of sufficient memory size (999 points along PC profile should be sufficient)
		if(profile == NULL){
			printf("Could not allocate profile memory.\n");
			exit(0);
		}
		profile->nmax = 999;
		profile->arr = malloc(sizeof(struct cap_prof_arrays)*(profile->nmax+1));
		if(profile->arr == NULL){
			printf("Could not allocate profile->arr memory.\n");
			exit(0);
                }

		//Define some general parameters
		profile->rtot1 = rad_ext[0];
		profile->rtot2 = rad_ext[1];
		profile->cl = length;
	        profile->binsize = 20.e-4;
	}

	//Define actual capillary and PC shape
	switch(shape){
		case 0: //conical
			for(i=0;i<profile->nmax;i++){
				profile->arr[i].zarr = length/profile->nmax*i; //z coordinates, from 0 to length
				profile->arr[i].profil = (rad_int[1]-rad_int[0])/length*profile->arr[i].zarr + rad_int[0]; //single capillary shape always conical
				profile->arr[i].sx = 0; //set sx and sy to 0 as they are overwritten in start() anyway. 
				profile->arr[i].sy = 0;	
				profile->arr[i].d_arr = (rad_ext[1]-rad_ext[0])/length*profile->arr[i].zarr + rad_ext[0];
			}
			break;

		case 1: //paraboloidal
			//determine points to be part of polycap external shape, based on focii and external radii
			pc_x[0] = 0.;
			pc_y[0] = rad_ext[0];
			pc_x[3] = length;
			pc_y[3] = rad_ext[1];
			if(focal_dist[0] <= length) pc_x[1] = focal_dist[0]/10.;
				else pc_x[1] = length/10.; 
			pc_y[1] = (rad_ext[0]-0.)/(0.-(-1.*focal_dist[0])) * (pc_x[1] - 0.) + rad_ext[0]; //extrapolate line between focus point and PC entrance
			if(focal_dist[1] <= length) pc_x[2] = length-focal_dist[0]/10.;
				else pc_x[2] = length-length/10.; 
			pc_y[2] = (rad_ext[1]-0.)/(length-(length+focal_dist[1])) * (pc_x[2] - length) + rad_ext[1]; //extrapolate line between focus point and PC exit
			polynomialfit(4, 3, pc_x, pc_y, coeff);

			//calculate shape coordinates
			for(i=0;i<profile->nmax;i++){
				profile->arr[i].zarr = length/profile->nmax*i; //z coordinates, from 0 to length
				profile->arr[i].profil = (rad_int[1]-rad_int[0])/length*profile->arr[i].zarr + rad_int[0]; //single capillary shape always conical
				profile->arr[i].sx = 0; //set sx and sy to 0 as they are overwritten in start() anyway. 
				profile->arr[i].sy = 0;	
				
				profile->arr[i].d_arr = coeff[0]+coeff[1]*profile->arr[i].zarr+coeff[2]*profile->arr[i].zarr*profile->arr[i].zarr;
			}
			break;

		case 2: //ellipsoidal; side with largest radius has horizontal tangent, other side points towards focal_dist corresponding to smallest external radius
			if(rad_ext[1] < rad_ext[0]){ //focussing alignment
				slope = rad_ext[1] / focal_dist[1];
				b = (-1.*(rad_ext[1]-rad_ext[0])*(rad_ext[1]-rad_ext[0])-slope*length*(rad_ext[1]-rad_ext[0])) / (slope*length+2.*(rad_ext[1]-rad_ext[0]));
				k = rad_ext[0] - b;
				a = sqrt((b*b*length)/(slope*(rad_ext[1]-k)));
				for(i=0;i<profile->nmax;i++){
					profile->arr[i].zarr = length/profile->nmax*i; //z coordinates, from 0 to length
					profile->arr[i].profil = (rad_int[1]-rad_int[0])/length*profile->arr[i].zarr + rad_int[0]; //single capillary shape always conical
					profile->arr[i].sx = 0; //set sx and sy to 0 as they are overwritten in start() anyway. 
					profile->arr[i].sy = 0;	
					profile->arr[i].d_arr = sqrt(b*b-(b*b*profile->arr[i].zarr*profile->arr[i].zarr)/(a*a))+k;
				}
			} else { //confocal (collimating) alignment
				slope = rad_ext[0] / focal_dist[0];
				b = (-1.*(rad_ext[0]-rad_ext[1])*(rad_ext[0]-rad_ext[1])-slope*length*(rad_ext[0]-rad_ext[1])) / (slope*length+2.*(rad_ext[0]-rad_ext[1]));
				k = rad_ext[1] - b;
				a = sqrt((b*b*length)/(slope*(rad_ext[0]-k)));
				for(i=0;i<profile->nmax;i++){
					profile->arr[i].zarr = length/profile->nmax*i; //z coordinates, from 0 to length
					profile->arr[i].profil = (rad_int[1]-rad_int[0])/length*profile->arr[i].zarr + rad_int[0]; //single capillary shape always conical
					profile->arr[i].sx = 0; //set sx and sy to 0 as they are overwritten in start() anyway. 
					profile->arr[i].sy = 0;	
				}
				j = profile->nmax-1;
				for(i=0;i<profile->nmax;i++){ //now profile->arr[].zarr array is properly defined, calculate d_arr
					profile->arr[i].d_arr = sqrt(b*b-(b*b*profile->arr[j].zarr*profile->arr[j].zarr)/(a*a))+k;
					j--;
				}
			}
			break;

		default:
			break;

	}
	return profile;

}

// ---------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------
// Main polycap program
int main(int argc, char *argv[])
	{
	struct inp_file cap;
	struct cap_profile *profile;
        int thread_max, thread_cnt;
	struct mumc *absmu;
	struct leakstruct *leaks;
	struct ini_polycap pcap_ini;
	int iesc_value,i,j;
	int *iesc = &iesc_value;
	struct image_struct *imstr;
	double *absorb_sum;
	double *sum_cnt;
	int icount=0;
	int64_t sum_refl=0, sum_istart=0, sum_ienter=0; //amount of reflected, started and entered photons
	double ave_refl; //average amount of reflections
	FILE *fptr; //pointer to access files
	double e=0;
	double dist=0;
	char f_abs[100];
	int arrsize=0;

	// Check whether input file argument was supplied
	if(argc <= 1){
		printf("Usage: polycap input-file should be supplied.\n");
		exit(0);
		}

	// Check maximal amount of threads and let user choose the amount of threads to use
	thread_max = omp_get_max_threads();
	printf("Type in the amount of threads to use (max %d):\n",thread_max);
	scanf("%d",&thread_cnt);
	printf("%d threads out of %d selected.\n",thread_cnt, thread_max);

	// Read *.inp file and save all information in cap structure;
	printf("Reading input file...");
	cap = read_cap_data(argv[1]);
	printf("   OK\n");
	
	// Read/create capillary profile data;
printf("cap.shape: %lf\n",cap.shape);
	if(cap.shape == 0 || cap.shape == 1 || cap.shape == 2){
		profile = def_cap_profile(cap.shape, cap.length, cap.rad_ext, cap.rad_int, cap.focal_dist);
		cap.d_screen = cap.d_screen + cap.d_source + profile->cl; //position of screen on z axis
	} else {
		printf("Reading capillary profile files...\n");
		profile = read_cap_profile(&cap);
		printf("Capillary profiles read.\n");
	}

	//Initialize
	absmu = ini_mumc(&cap);
	leaks = reset_leak(profile,absmu);
	pcap_ini = ini_polycap(&cap,profile);

	//allocate memory to imstr
	imstr = malloc(sizeof(struct image_struct)*IMSIZE);
	if(imstr == NULL){
		printf("Could not allocate imstr memory.\n");
		exit(0);
		}

	*iesc = 0;
	for(i=0; i<= IMSIZE-1; i++){
		imstr[i].xsou  = 0;
		imstr[i].ysou  = 0;
		imstr[i].xsou1 = 0;
		imstr[i].ysou1 = 0;
		imstr[i].wsou  = 0;
		imstr[i].xm    = 0;
		imstr[i].ym    = 0;
		imstr[i].xm1   = 0;
		imstr[i].ym1   = 0;
		imstr[i].warr  = 0;
		}

	printf("Starting calculations...\n");

	absorb_sum = malloc(sizeof(*absorb_sum)*(profile->nmax+1));
	if(absorb_sum == NULL){
		printf("Could not allocate absorb_sum memory.\n");
		exit(0);
	}
	for(j=0; j<=profile->nmax; j++){
		absorb_sum[j] = 0.;
	}
	sum_cnt = malloc(sizeof(*sum_cnt)*(absmu->n_energy+1));
	if(sum_cnt == NULL){
		printf("Could not allocate sum_cnt memory.\n");
		exit(0);
	}
	for(j=0; j<=absmu->n_energy;j++){
		sum_cnt[j] = 0.;
	}

#ifndef _WIN32
	FILE *random_device;
	if ((random_device = fopen("/dev/urandom", "r")) == NULL) {
		printf("Could not open /dev/urandom");
		exit(0);
	}
#endif
	
	unsigned long int *seeds = malloc(sizeof(unsigned long int) * thread_cnt);
	for(i=0;i<thread_cnt;i++){
#ifdef _WIN32
		rand_s(&seeds[i]);
#else
		fread(&seeds[i], sizeof(unsigned long int), 1, random_device);
#endif
	}
#ifndef _WIN32
	fclose(random_device);
#endif

	//Actual multi-core loop where the calculations happen.
	#pragma omp parallel \
		    default(shared) \
		    private(icount,i,j) \
		    firstprivate(cap,profile,absmu,leaks,pcap_ini,thread_cnt) \
		    num_threads(thread_cnt) \
		    reduction(+:sum_refl,sum_istart,sum_ienter)
	{
		int thread_id = omp_get_thread_num();
		struct calcstruct *calc = init_calcstruct(seeds[thread_id], profile, absmu);
		i = 0;
		#pragma omp for nowait
		for(icount=0 ; icount <= cap.ndet ; icount++) {
			do {
				do {
					start(absmu, profile, &pcap_ini, &cap, icount, imstr, calc);
					do {
						capil(absmu, profile, &cap, leaks, calc);
					} while(calc->iesc == 0);
				} while(calc->iesc == -2);
				count(absmu, &cap, icount, profile, leaks, imstr,calc);
			} while(calc->iesc == -3);
			sum_refl += calc->i_refl;
			if(thread_id == 0 && (double)i/((double)cap.ndet/(double)thread_cnt/10.) >= 1.){
				printf("%d%%\t%lld\t%f\n",((icount*100)/(cap.ndet/thread_cnt)),calc->i_refl,calc->rh[2]);
				i=0;
			}
			i++;//counter just to follow % completed
		} //for(icount=0; icount <= cap.ndet; icount++)

		sum_istart += calc->istart;
		sum_ienter += calc->ienter;

		#pragma omp critical
		{
			for(j=0; j <= profile->nmax; j++){
				absorb_sum[j] += calc->absorb[j];
			}
			for(j=0; j <= absmu->n_energy; j++){
				sum_cnt[j] += calc->cnt[j];
			}
		}

		// free memory
		polycap_rng_free(calc->rn);
		free(calc->sx);
		free(calc->sy);
		free(calc->absorb);
		free(calc->w);
		free(calc->cnt);
		free(calc);
	}

	ave_refl = (double)sum_refl/(double)cap.ndet;
	printf("Average number of reflections: %f\n",ave_refl);


	// Output writing
	fptr = fopen("xy.dat","w"); //stores coordinates of photon on screen(xm, ym), as well as direction(xm1,ym1)
	if(IMSIZE > cap.ndet) arrsize = cap.ndet+1;
		 else arrsize = IMSIZE;
	fprintf(fptr,"%d\n",arrsize);
	fprintf(fptr,"%f\n",e);
	fprintf(fptr,"%f\n",cap.e_start);
	fprintf(fptr,"%f\n",dist);
	for(i=0; i<arrsize; i++){
		fprintf(fptr,"%f\t%f\t%f\t%f\t%f\n",imstr[i].xm,imstr[i].xm1,imstr[i].ym,imstr[i].ym1,imstr[i].warr);
		}
	fclose(fptr);

	fptr = fopen("xys.dat","w"); //coordinates and direction of photon from source origin
	fprintf(fptr,"%d\n",arrsize);
	fprintf(fptr,"%f\n",e);
        fprintf(fptr,"%f\n",cap.e_start);
        fprintf(fptr,"%f\n",dist);
        for(i=0; i<arrsize; i++){
		fprintf(fptr,"%f\t%f\t%f\t%f\t%f\n",imstr[i].xsou,imstr[i].xsou1,imstr[i].ysou,imstr[i].ysou1,imstr[i].wsou);
		}
	fclose(fptr);

	fptr = fopen("spot.dat","w");
	fprintf(fptr,"%d\t%d\n",NSPOT,NSPOT);
	for(j=0; j<NSPOT; j++){
		for(i=0; i<NSPOT; i++){
			fprintf(fptr,"%f\t",leaks->spot[i][j]);
			}
		fprintf(fptr,"\n");
		}
	fclose(fptr);
	
	fptr = fopen("lspot.dat","w");
	fprintf(fptr,"%d\t%d\n",NSPOT,NSPOT);
	for(j=0; j<NSPOT; j++){
		for(i=0; i<NSPOT; i++){
			fprintf(fptr,"%f\t",leaks->lspot[i][j]);
			}
		fprintf(fptr,"\n");
		}
	fclose(fptr);

	fptr = fopen(cap.out,"w");
	if(fptr == NULL){
		printf("Trouble with output...\n");
		exit(0);
		}
	fprintf(fptr,"Surface roughness [Angstrom]:\t %f\n",cap.sig_rough);
	fprintf(fptr,"Amplitude of Waviness [cm]:\t %f\n",cap.sig_wave);
	fprintf(fptr,"Waviness corr. length [cm]:\t %f\n",cap.corr_length);
	fprintf(fptr,"Source distance [cm]:\t\t %f\n",cap.d_source);
	fprintf(fptr,"Screen distance [cm]:\t\t %f\n",cap.d_screen);
	fprintf(fptr,"Source diameter [cm]:\t\t %f\n",cap.src_x*2.);
	fprintf(fptr,"Capillary foc. distances [cm]:\t %f\t%f\n",cap.src_sigx,cap.src_sigy);//this is not what's written here...
	fprintf(fptr,"Number of channels:\t\t %5.0f\n",cap.n_chan);
	fprintf(fptr,"Calculated capillary open area:\t %5.3f\n",pcap_ini.eta);
	fprintf(fptr,"Misalignment rotation [rad]/translation [cm]: %f\t%f\n",cap.src_shiftx,cap.src_shifty); //only translation
	fprintf(fptr,"Capillary profile: %s\n",cap.prf);
	fprintf(fptr,"Capillary axis   : %s\n",cap.axs);
	fprintf(fptr,"External profile : %s\n",cap.ext);
	fprintf(fptr,"Input file       : %s\n",argv[1]);
	fprintf(fptr,"  E [keV]      I/I0\n");
	fprintf(fptr,"$DATA:\n");
	fprintf(fptr,"%d\t%d\n",absmu->n_energy+1,5);
	for(i=0; i<=absmu->n_energy; i++){
		fprintf(fptr,"%8.2f\t%10.9f\t%10.9f\t%10.9f\t%10.9f\n",cap.e_start+i*cap.delta_e,
			sum_cnt[i]/(double)sum_ienter*pcap_ini.eta, sum_cnt[i]/(double)sum_istart,
			(double)sum_ienter/(double)sum_istart, leaks->leak[i]/(double)sum_ienter);
		}
	fprintf(fptr,"\nThe started photons: %lld\n",sum_istart);
	fprintf(fptr,"\nAverage number of reflections: %f\n",ave_refl);
	fclose(fptr);

	sprintf(f_abs,"%s.abs",cap.out);
	fptr = fopen(f_abs,"w");
	fprintf(fptr,"$DATA:\n");
	fprintf(fptr,"%d\t%d\n",profile->nmax,2);
	for(i=0;i<=profile->nmax;i++) fprintf(fptr,"%f\t%f\n",profile->arr[i].zarr,absorb_sum[i]);
	fclose(fptr);


	// free allocated memory
	free(profile->arr);
	free(profile);
	free(imstr);
	free(absorb_sum);
	free(sum_cnt);
	free(absmu->arr);
	free(absmu);
	free(leaks->leak);
	free(leaks);
	return 0;
	}
// ---------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------
//
//
//
//
//
//
//
//
//
//
//
//
//
//
