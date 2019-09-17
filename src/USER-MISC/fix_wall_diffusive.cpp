/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

#include "fix_wall_diffusive.h"
#include <cstring>
#include "atom.h"
#include "comm.h"
#include "update.h"
#include "modify.h"
#include "domain.h"
#include "lattice.h"
#include "input.h"
#include "variable.h"
#include "error.h"
#include "force.h"
#include "random_mars.h"
//#include "random_park.h"
#include <stdlib.h> 
//#include <iostream> 

using namespace LAMMPS_NS;
using namespace FixConst;
//using namespace std; 

enum{XLO=0,XHI=1,YLO=2,YHI=3,ZLO=4,ZHI=5};
enum{NONE=0,EDGE,CONSTANT,VARIABLE};

/* ---------------------------------------------------------------------- */

FixWallDiffusive::FixWallDiffusive(LAMMPS *lmp, int narg, char **arg) :
  Fix(lmp, narg, arg), random(NULL),
  nwall(0)
{
  if (narg < 4) error->all(FLERR,"Illegal fix wall/diffusive command");

  dynamic_group_allow = 1;

  // parse args

  nwall = 0;
  int scaleflag = 1;
  //RanMars *random;

  do seedfix = rand(); while (seedfix > 900000000);
  random = new RanMars(lmp,seedfix);

  int iarg = 3;
  while (iarg < narg) {
    if ((strcmp(arg[iarg],"xlo") == 0) || (strcmp(arg[iarg],"xhi") == 0) ||
        (strcmp(arg[iarg],"ylo") == 0) || (strcmp(arg[iarg],"yhi") == 0) ||
        (strcmp(arg[iarg],"zlo") == 0) || (strcmp(arg[iarg],"zhi") == 0)) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal fix wall/diffusive command");

      int newwall;
      if (strcmp(arg[iarg],"xlo") == 0) newwall = XLO;
      else if (strcmp(arg[iarg],"xhi") == 0) newwall = XHI;
      else if (strcmp(arg[iarg],"ylo") == 0) newwall = YLO;
      else if (strcmp(arg[iarg],"yhi") == 0) newwall = YHI;
      else if (strcmp(arg[iarg],"zlo") == 0) newwall = ZLO;
      else if (strcmp(arg[iarg],"zhi") == 0) newwall = ZHI;

      for (int m = 0; (m < nwall) && (m < 6); m++)
        if (newwall == wallwhich[m])
          error->all(FLERR,"Wall defined twice in fix wall/diffusive command");

      wallwhich[nwall] = newwall;
      if (strcmp(arg[iarg+1],"EDGE") == 0) {
        wallstyle[nwall] = EDGE;
        int dim = wallwhich[nwall] / 2;
        int side = wallwhich[nwall] % 2;
        if (side == 0) coord0[nwall] = domain->boxlo[dim];
        else coord0[nwall] = domain->boxhi[dim];
      } else if (strstr(arg[iarg+1],"v_") == arg[iarg+1]) {
        wallstyle[nwall] = VARIABLE;
        int n = strlen(&arg[iarg+1][2]) + 1;
        varstr[nwall] = new char[n];
        strcpy(varstr[nwall],&arg[iarg+1][2]);
      } else {
        wallstyle[nwall] = CONSTANT;
        coord0[nwall] = force->numeric(FLERR,arg[iarg+1]);
      }
      walltemp[nwall]= force->numeric(FLERR,arg[iarg+2]);
      wallvel[nwall][0]= force->numeric(FLERR,arg[iarg+3]);
      wallvel[nwall][1]= force->numeric(FLERR,arg[iarg+4]);
      wallvel[nwall][2]= force->numeric(FLERR,arg[iarg+5]);

      nwall++;
      iarg += 6;

    } else if (strcmp(arg[iarg],"units") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal wall/diffusive command");
      if (strcmp(arg[iarg+1],"box") == 0) scaleflag = 0;
      else if (strcmp(arg[iarg+1],"lattice") == 0) scaleflag = 1;
      else error->all(FLERR,"Illegal fix wall/diffusive command");
      iarg += 2;
    } else error->all(FLERR,"Illegal fix wall/diffusive command");
  }

  // error check

  if (nwall == 0) error->all(FLERR,"Illegal fix wall command");

  for (int m = 0; m < nwall; m++) {
    if ((wallwhich[m] == XLO || wallwhich[m] == XHI) && domain->xperiodic)
      error->all(FLERR,"Cannot use fix wall/diffusive in periodic dimension");
    if ((wallwhich[m] == YLO || wallwhich[m] == YHI) && domain->yperiodic)
      error->all(FLERR,"Cannot use fix wall/diffusive in periodic dimension");
    if ((wallwhich[m] == ZLO || wallwhich[m] == ZHI) && domain->zperiodic)
      error->all(FLERR,"Cannot use fix wall/diffusive in periodic dimension");
  }

  for (int m = 0; m < nwall; m++)
    if ((wallwhich[m] == ZLO || wallwhich[m] == ZHI) && domain->dimension == 2)
      error->all(FLERR,
                 "Cannot use fix wall/diffusive zlo/zhi for a 2d simulation");

  // scale factors for CONSTANT and VARIABLE walls

  int flag = 0;
  for (int m = 0; m < nwall; m++)
    if (wallstyle[m] != EDGE) flag = 1;

  if (flag) {
    if (scaleflag) {
      xscale = domain->lattice->xlattice;
      yscale = domain->lattice->ylattice;
      zscale = domain->lattice->zlattice;
    }
    else xscale = yscale = zscale = 1.0;

    for (int m = 0; m < nwall; m++) {
      if (wallstyle[m] != CONSTANT) continue;
      if (wallwhich[m] < YLO) coord0[m] *= xscale;
      else if (wallwhich[m] < ZLO) coord0[m] *= yscale;
      else coord0[m] *= zscale;
    }
  }

  // set varflag if any wall positions are variable

  varflag = 0;
  for (int m = 0; m < nwall; m++)
    if (wallstyle[m] == VARIABLE) varflag = 1;
}

/* ---------------------------------------------------------------------- */

FixWallDiffusive::~FixWallDiffusive()
{
  if (copymode) return;

  for (int m = 0; m < nwall; m++)
    if (wallstyle[m] == VARIABLE) delete [] varstr[m];
    delete random;
}

/* ---------------------------------------------------------------------- */

int FixWallDiffusive::setmask()
{
  int mask = 0;
  mask |= POST_INTEGRATE;
  mask |= POST_INTEGRATE_RESPA;
  return mask;
}

/* ---------------------------------------------------------------------- */

void FixWallDiffusive::init()
{
  for (int m = 0; m < nwall; m++) {
    if (wallstyle[m] != VARIABLE) continue;
    varindex[m] = input->variable->find(varstr[m]);
    if (varindex[m] < 0)
      error->all(FLERR,"Variable name for fix wall/diffusive does not exist");
    if (!input->variable->equalstyle(varindex[m]))
      error->all(FLERR,"Variable for fix wall/diffusive is invalid style");
  }

  int nrigid = 0;
  for (int i = 0; i < modify->nfix; i++)
    if (modify->fix[i]->rigid_flag) nrigid++;

  if (nrigid && comm->me == 0)
    error->warning(FLERR,"Should not allow rigid bodies to bounce off "
                   "relecting walls");
}


/* ---------------------------------------------------------------------- */

void FixWallDiffusive::post_integrate()
{
  int i,dir,dim,side;
  double coord,vsave,factor,time2;

  
  double *rmass;
  double *mass = atom->mass;


  // coord = current position of wall
  // evaluate variable if necessary, wrap with clear/add


  double **x = atom->x;
  double **v = atom->v;
  int *mask = atom->mask;
  int *type = atom->type;
  int nlocal = atom->nlocal;
  

  
  if (varflag) modify->clearstep_compute();

  for (int m = 0; m < nwall; m++) {
    if (wallstyle[m] == VARIABLE) {
      coord = input->variable->compute_equal(varindex[m]);
      if (wallwhich[m] < YLO) coord *= xscale;
      else if (wallwhich[m] < ZLO) coord *= yscale;
      else coord *= zscale;
    } else coord = coord0[m];

    dim = wallwhich[m] / 2;
    side = wallwhich[m] % 2;
  
    for (i = 0; i < nlocal; i++) 
      if (mask[i] & groupbit) {
        if (side == 0) {
          if (x[i][dim] < coord) {
           if (rmass) factor = sqrt(force->boltz*walltemp[m]/(rmass[i]*force->mvv2e));
           else factor = sqrt(force->boltz*walltemp[m]/(mass[type[i]]*force->mvv2e));
             
             time2= (x[i][dim] - coord)/(v[i][dim]-wallvel[m][dim]);// after collision
           

            for (dir = 0; dir < 3; dir++) 
            if (dir != dim) {
            x[i][dir] -=v[i][dir]*time2;
            v[i][dir] = wallvel[m][dir] + random->gaussian(0,factor) ;
            x[i][dir] +=v[i][dir]*time2;
            cout << v[i][dir] << "\n";

            } else {
            v[i][dir] = wallvel[m][dir] + random->rayleigh(factor) ; 
            x[i][dir] = coord + v[i][dir]*time2;
            cout << v[i][dir] << "\n";
            }
            

          }
        } else {
          if (x[i][dim] > coord) {
           if (rmass) factor = sqrt(force->boltz*walltemp[m]/(rmass[i]*force->mvv2e));
           else factor = sqrt(force->boltz*walltemp[m]/(mass[type[i]]*force->mvv2e));

             time2= (x[i][dim] - coord)/(v[i][dim]-wallvel[m][dim]);// after collision


            for (dir = 0; dir < 3; dir++) 
            if (dir != dim) {
            x[i][dir] -=v[i][dir]*time2;
            v[i][dir] = wallvel[m][dir] + random->gaussian(0,factor) ;
            x[i][dir] +=v[i][dir]*time2;
            cout << v[i][dir] << "\n";
            
            } else {
            v[i][dir] = wallvel[m][dir] - random->rayleigh(factor) ; 
            x[i][dir] = coord + v[i][dir]*time2;
            cout << v[i][dir] << "\n";
            
            }


          }
        }
      }
  }

  if (varflag) modify->addstep_compute(update->ntimestep + 1);
}
