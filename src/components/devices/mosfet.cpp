/*
 * mosfet.cpp - mosfet class implementation
 *
 * Copyright (C) 2004, 2005, 2006 Stefan Jahn <stefan@lkcc.org>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this package; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street - Fifth Floor,
 * Boston, MA 02110-1301, USA.  
 *
 * $Id: mosfet.cpp,v 1.37 2008-01-10 20:00:01 ela Exp $
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "complex.h"
#include "matrix.h"
#include "object.h"
#include "logging.h"
#include "node.h"
#include "circuit.h"
#include "net.h"
#include "component_id.h"
#include "constants.h"
#include "device.h"
#include "mosfet.h"

#define NODE_G 0 /* gate node   */
#define NODE_D 1 /* drain node  */
#define NODE_S 2 /* source node */
#define NODE_B 3 /* bulk node   */

using namespace device;

mosfet::mosfet () : circuit (4) {
  transientMode = 0;
  rg = rs = rd = NULL;
  type = CIR_MOSFET;
}

void mosfet::calcSP (nr_double_t frequency) {
  setMatrixS (ytos (calcMatrixY (frequency)));
}

matrix mosfet::calcMatrixY (nr_double_t frequency) {

  // fetch computed operating points
  nr_double_t Cgd = getOperatingPoint ("Cgd");
  nr_double_t Cgs = getOperatingPoint ("Cgs");
  nr_double_t Cbd = getOperatingPoint ("Cbd");
  nr_double_t Cbs = getOperatingPoint ("Cbs");
  nr_double_t Cgb = getOperatingPoint ("Cgb");
  nr_double_t gbs = getOperatingPoint ("gbs");
  nr_double_t gbd = getOperatingPoint ("gbd");
  nr_double_t gds = getOperatingPoint ("gds");
  nr_double_t gm  = getOperatingPoint ("gm");
  nr_double_t gmb = getOperatingPoint ("gmb");

  // compute the models admittances
  nr_complex_t Ygd = rect (0.0, 2.0 * M_PI * frequency * Cgd);
  nr_complex_t Ygs = rect (0.0, 2.0 * M_PI * frequency * Cgs);
  nr_complex_t Yds = gds;
  nr_complex_t Ybd = rect (gbd, 2.0 * M_PI * frequency * Cbd);
  nr_complex_t Ybs = rect (gbs, 2.0 * M_PI * frequency * Cbs);
  nr_complex_t Ygb = rect (0.0, 2.0 * M_PI * frequency * Cgb);

  // build admittance matrix and convert it to S-parameter matrix
  matrix y (4);
  y.set (NODE_G, NODE_G, Ygd + Ygs + Ygb);
  y.set (NODE_G, NODE_D, -Ygd);
  y.set (NODE_G, NODE_S, -Ygs);
  y.set (NODE_G, NODE_B, -Ygb);
  y.set (NODE_D, NODE_G, gm - Ygd);
  y.set (NODE_D, NODE_D, Ygd + Yds + Ybd - DrainControl);
  y.set (NODE_D, NODE_S, -Yds - SourceControl);
  y.set (NODE_D, NODE_B, -Ybd + gmb);
  y.set (NODE_S, NODE_G, -Ygs - gm);
  y.set (NODE_S, NODE_D, -Yds + DrainControl);
  y.set (NODE_S, NODE_S, Ygs + Yds + Ybs + SourceControl);
  y.set (NODE_S, NODE_B, -Ybs - gmb);
  y.set (NODE_B, NODE_G, -Ygb);
  y.set (NODE_B, NODE_D, -Ybd);
  y.set (NODE_B, NODE_S, -Ybs);
  y.set (NODE_B, NODE_B, Ybd + Ybs + Ygb);

  return y;
}

void mosfet::calcNoiseSP (nr_double_t frequency) {
  setMatrixN (cytocs (calcMatrixCy (frequency) * z0, getMatrixS ()));
}

matrix mosfet::calcMatrixCy (nr_double_t frequency) {
  /* get operating points and noise properties */
  nr_double_t Kf  = getPropertyDouble ("Kf");
  nr_double_t Af  = getPropertyDouble ("Af");
  nr_double_t Ffe = getPropertyDouble ("Ffe");
  nr_double_t gm  = fabs (getOperatingPoint ("gm"));
  nr_double_t Ids = fabs (getOperatingPoint ("Id"));
  nr_double_t T   = getPropertyDouble ("Temp");

  /* compute channel noise and flicker noise generated by the DC
     transconductance and current flow from drain to source */
  nr_double_t i = 8 * kelvin (T) / T0 * gm / 3 +
    Kf * pow (Ids, Af) / pow (frequency, Ffe) / kB / T0;

  /* build noise current correlation matrix and convert it to
     noise-wave correlation matrix */
  matrix cy = matrix (4);
  cy.set (NODE_D, NODE_D, +i);
  cy.set (NODE_S, NODE_S, +i);
  cy.set (NODE_D, NODE_S, -i);
  cy.set (NODE_S, NODE_D, -i);
  return cy;
}

void mosfet::restartDC (void) {
  // apply starting values to previous iteration values
  UgdPrev = real (getV (NODE_G) - getV (NODE_D));
  UgsPrev = real (getV (NODE_G) - getV (NODE_S));
  UbsPrev = real (getV (NODE_B) - getV (NODE_S));
  UbdPrev = real (getV (NODE_B) - getV (NODE_D));
  UdsPrev = UgsPrev - UgdPrev;
}

void mosfet::initDC (void) {

  // allocate MNA matrices
  allocMatrixMNA ();

  // initialize starting values
  restartDC ();

  // initialize the MOSFET
  initModel ();

  // get device temperature
  nr_double_t T = getPropertyDouble ("Temp");

  // possibly insert series resistance at source
  if (Rs != 0.0) {
    // create additional circuit if necessary and reassign nodes
    rs = splitResistor (this, rs, "Rs", "source", NODE_S);
    rs->setProperty ("Temp", T);
    rs->setProperty ("R", Rs);
    rs->setProperty ("Controlled", getName ());
    rs->initDC ();
  }
  // no series resistance at source
  else {
    disableResistor (this, rs, NODE_S);
  }

  // possibly insert series resistance at gate
  nr_double_t Rg = getPropertyDouble ("Rg");
  if (Rg != 0.0) {
    // create additional circuit if necessary and reassign nodes
    rg = splitResistor (this, rg, "Rg", "gate", NODE_G);
    rg->setProperty ("Temp", T);
    rg->setProperty ("R", Rg);
    rg->setProperty ("Controlled", getName ());
    rg->initDC ();
  }
  // no series resistance at source
  else {
    disableResistor (this, rg, NODE_G);
  }

  // possibly insert series resistance at drain
  if (Rd != 0.0) {
    // create additional circuit if necessary and reassign nodes
    rd = splitResistor (this, rd, "Rd", "drain", NODE_D);
    rd->setProperty ("Temp", T);
    rd->setProperty ("R", Rd);
    rd->setProperty ("Controlled", getName ());
    rd->initDC ();
  }
  // no series resistance at drain
  else {
    disableResistor (this, rd, NODE_D);
  }
}

void mosfet::initModel (void) {

  // get device temperature
  nr_double_t T  = getPropertyDouble ("Temp");
  nr_double_t T2 = kelvin (getPropertyDouble ("Temp"));
  nr_double_t T1 = kelvin (getPropertyDouble ("Tnom"));

  // apply polarity of MOSFET
  char * type = getPropertyString ("Type");
  pol = !strcmp (type, "pfet") ? -1 : 1;

  // calculate effective channel length
  nr_double_t L  = getPropertyDouble ("L");
  nr_double_t Ld = getPropertyDouble ("Ld");
  if ((Leff = L - 2 * Ld) <= 0) {
    logprint (LOG_STATUS, "WARNING: effective MOSFET channel length %g <= 0, "
	      "set to L = %g\n", Leff, L);
    Leff = L;
  }

  // calculate gate oxide overlap capacitance
  nr_double_t W   = getPropertyDouble ("W");
  nr_double_t Tox = getPropertyDouble ("Tox");
  if (Tox <= 0) {
    logprint (LOG_STATUS, "WARNING: disabling gate oxide capacitance, "
	      "Cox = 0\n");
    Cox = 0;
  } else {
    Cox = (ESiO2 * E0 / Tox);
  }

  // calculate DC transconductance coefficient
  nr_double_t Kp = getPropertyDouble ("Kp");
  nr_double_t Uo = getPropertyDouble ("Uo");
  nr_double_t F1 = exp (1.5 * log (T1 / T2));
  Kp = Kp * F1;
  Uo = Uo * F1;
  setScaledProperty ("Kp", Kp);
  setScaledProperty ("Uo", Uo);
  if (Kp > 0) {
    beta = Kp * W / Leff;
  } else {
    if (Cox > 0 && Uo > 0) {
      beta = Uo * 1e-4 * Cox * W / Leff;
    } else {
      logprint (LOG_STATUS, "WARNING: adjust Tox, Uo or Kp to get a valid "
		"transconductance coefficient\n");
      beta = 2e-5 * W / Leff;
    }
  }

  // calculate surface potential
  nr_double_t P    = getPropertyDouble ("Phi");
  nr_double_t Nsub = getPropertyDouble ("Nsub");
  nr_double_t Ut   = T0 * kBoverQ;
  P = pnPotential_T (T1,T2, P);
  setScaledProperty ("Phi", P);
  if ((Phi = P) <= 0) {
    if (Nsub > 0) {
      if (Nsub * 1e6 >= NiSi) {
	Phi = 2 * Ut * log (Nsub * 1e6 / NiSi);
      } else {
	logprint (LOG_STATUS, "WARNING: substrate doping less than instrinsic "
		  "density, adjust Nsub >= %g\n", NiSi / 1e6);
	Phi = 0.6;
      }
    } else {
      logprint (LOG_STATUS, "WARNING: adjust Nsub or Phi to get a valid "
		"surface potential\n");
      Phi = 0.6;
    }
  }

  // calculate bulk threshold
  nr_double_t G = getPropertyDouble ("Gamma");
  if ((Ga = G) < 0) {
    if (Cox > 0 && Nsub > 0) {
      Ga = sqrt (2 * Q * ESi * E0 * Nsub * 1e6) / Cox;
    } else {
      logprint (LOG_STATUS, "WARNING: adjust Tox, Nsub or Gamma to get a "
		"valid bulk threshold\n");
      Ga = 0.0;
    }
  }

  // calculate threshold voltage
  nr_double_t Vt0 = getPropertyDouble ("Vt0");
  if ((Vto = Vt0) == 0.0) {
    nr_double_t Tpg = getPropertyDouble ("Tpg");
    nr_double_t Nss = getPropertyDouble ("Nss");
    nr_double_t PhiMS, PhiG, Eg;
    // bandgap for silicon
    Eg = Egap (kelvin (T));
    if (Tpg != 0.0) { // n-poly or p-poly
      PhiG = 4.15 + Eg / 2 - pol * Tpg * Eg / 2;
    } else {          // alumina
      PhiG = 4.1;
    }
    PhiMS = PhiG - (4.15 + Eg / 2 + pol * Phi / 2);
    if (Nss >= 0 && Cox > 0) {
      Vto = PhiMS - Q * Nss * 1e4 / Cox + pol * (Phi + Ga * sqrt (Phi));
    } else {
      logprint (LOG_STATUS, "WARNING: adjust Tox, Nss or Vt0 to get a "
		"valid threshold voltage\n");
      Vto = 0.0;
    }
  }

  Cox = Cox * W * Leff;

  // calculate drain and source resistance if necessary
  nr_double_t Rsh = getPropertyDouble ("Rsh");
  nr_double_t Nrd = getPropertyDouble ("Nrd");
  nr_double_t Nrs = getPropertyDouble ("Nrs");
  Rd = getPropertyDouble ("Rd");
  Rs = getPropertyDouble ("Rs");
  if (Rsh > 0) {
    if (Nrd > 0) Rd += Rsh * Nrd;
    if (Nrs > 0) Rs += Rsh * Nrs;
  }

  // calculate zero-bias junction capacitance
  nr_double_t Cj  = getPropertyDouble ("Cj");
  nr_double_t Mj  = getPropertyDouble ("Mj");
  nr_double_t Mjs = getPropertyDouble ("Mjsw");
  nr_double_t Pb  = getPropertyDouble ("Pb");
  nr_double_t PbT, F2, F3;
  PbT = pnPotential_T (T1,T2, Pb);
  F2  = pnCapacitance_F (T1, T2, Mj, PbT / Pb);
  F3  = pnCapacitance_F (T1, T2, Mjs, PbT / Pb);
  Pb  = PbT;
  setScaledProperty ("Pb", Pb);
  if (Cj <= 0) {
    if (Pb > 0 && Nsub >= 0) {
      Cj = sqrt (ESi * E0 * Q * Nsub * 1e6 / 2 / Pb);
    }
    else {
      logprint (LOG_STATUS, "WARNING: adjust Pb, Nsub or Cj to get a "
		"valid square junction capacitance\n");
      Cj = 0.0;
    }
  }
  Cj = Cj * F2;
  setScaledProperty ("Cj", Cj);

  // calculate junction capacitances
  nr_double_t Cbd0 = getPropertyDouble ("Cbd");
  nr_double_t Cbs0 = getPropertyDouble ("Cbs");
  nr_double_t Ad   = getPropertyDouble ("Ad");
  nr_double_t As   = getPropertyDouble ("As");
  Cbd0 = Cbd0 * F2;
  if (Cbd0 <= 0) {
    Cbd0 = Cj * Ad;
  }
  setScaledProperty ("Cbd", Cbd0);
  Cbs0 = Cbs0 * F2;
  if (Cbs0 <= 0) {
    Cbs0 = Cj * As;
  }
  setScaledProperty ("Cbs", Cbs0);

  // calculate periphery junction capacitances
  nr_double_t Cjs = getPropertyDouble ("Cjsw");
  nr_double_t Pd  = getPropertyDouble ("Pd");
  nr_double_t Ps  = getPropertyDouble ("Ps");
  Cjs = Cjs * F3;
  setProperty ("Cbds", Cjs * Pd);
  setProperty ("Cbss", Cjs * Ps);

  // calculate junction capacitances and saturation currents
  nr_double_t Js  = getPropertyDouble ("Js");
  nr_double_t Is  = getPropertyDouble ("Is");
  nr_double_t F4, E1, E2;
  E1 = Egap (T1);
  E2 = Egap (T2);
  F4 = exp (- QoverkB / T2 * (T2 / T1 * E1 - E2));
  Is = Is * F4;
  Js = Js * F4;
  nr_double_t Isd = (Ad > 0) ? Js * Ad : Is;
  nr_double_t Iss = (As > 0) ? Js * As : Is;
  setProperty ("Isd", Isd);
  setProperty ("Iss", Iss);

#if DEBUG
  logprint (LOG_STATUS, "NOTIFY: Cox=%g, Beta=%g Ga=%g, Phi=%g, Vto=%g\n",
	    Cox, beta, Ga, Phi, Vto);
#endif /* DEBUG */
}

void mosfet::calcDC (void) {

  // fetch device model parameters
  nr_double_t Isd = getPropertyDouble ("Isd");
  nr_double_t Iss = getPropertyDouble ("Iss");
  nr_double_t n   = getPropertyDouble ("N");
  nr_double_t l   = getPropertyDouble ("Lambda");
  nr_double_t T   = getPropertyDouble ("Temp");

  nr_double_t Ut, IeqBS, IeqBD, IeqDS, UbsCrit, UbdCrit, gtiny;

  T = kelvin (T);
  Ut = T * kBoverQ;
  Ugd = real (getV (NODE_G) - getV (NODE_D)) * pol;
  Ugs = real (getV (NODE_G) - getV (NODE_S)) * pol;
  Ubs = real (getV (NODE_B) - getV (NODE_S)) * pol;
  Ubd = real (getV (NODE_B) - getV (NODE_D)) * pol;
  Uds = Ugs - Ugd;

  // critical voltage necessary for bad start values
  UbsCrit = pnCriticalVoltage (Iss, Ut * n);
  UbdCrit = pnCriticalVoltage (Isd, Ut * n);

  // for better convergence
  if (Uds >= 0) {
    Ugs = fetVoltage (Ugs, UgsPrev, Vto * pol);
    Uds = Ugs - Ugd;
    Uds = fetVoltageDS (Uds, UdsPrev);
    Ugd = Ugs - Uds;
  }
  else {
    Ugd = fetVoltage (Ugd, UgdPrev, Vto * pol);
    Uds = Ugs - Ugd;
    Uds = -fetVoltageDS (-Uds, -UdsPrev);
    Ugs = Ugd + Uds;
  }
  if (Uds >= 0) {
    Ubs = pnVoltage (Ubs, UbsPrev, Ut * n, UbsCrit);
    Ubd = Ubs - Uds;
  }
  else {
    Ubd = pnVoltage (Ubd, UbdPrev, Ut * n, UbdCrit);
    Ubs = Ubd + Uds;
  }
  UgsPrev = Ugs; UgdPrev = Ugd; UbdPrev = Ubd; UdsPrev = Uds; UbsPrev = Ubs;

  // parasitic bulk-source diode
  gtiny = Iss;
  pnJunctionMOS (Ubs, Iss, Ut * n, Ibs, gbs);
  Ibs += gtiny * Ubs;
  gbs += gtiny;

  // parasitic bulk-drain diode
  gtiny = Isd;
  pnJunctionMOS (Ubd, Isd, Ut * n, Ibd, gbd);
  Ibd += gtiny * Ubd;
  gbd += gtiny;

  // differentiate inverse and forward mode
  MOSdir = (Uds >= 0) ? +1 : -1;

  // first calculate sqrt (Upn - Phi)
  nr_double_t Upn = (MOSdir > 0) ? Ubs : Ubd;
  nr_double_t Sarg, Sphi = sqrt (Phi);
  if (Upn <= 0) {
    // take equation as is
    Sarg = sqrt (Phi - Upn);
  }
  else {
    // taylor series of "sqrt (x - 1)" -> continual at Ubs/Ubd = 0
    Sarg = Sphi - Upn / Sphi / 2;
    Sarg = MAX (Sarg, 0);
  }

  // calculate bias-dependent threshold voltage
  Uon = Vto * pol + Ga * (Sarg - Sphi);
  nr_double_t Utst = ((MOSdir > 0) ? Ugs : Ugd) - Uon;
  // no infinite backgate transconductance (if non-zero Ga)
  nr_double_t arg = (Sarg != 0.0) ? (Ga / Sarg / 2) : 0;

  // cutoff region
  if (Utst <= 0) {
    Ids = 0;
    gm  = 0;
    gds = 0;
    gmb = 0;
  }
  else {
    nr_double_t Vds = Uds * MOSdir;
    nr_double_t b   = beta * (1 + l * Vds);
    // saturation region
    if (Utst <= Vds) {
      Ids = b * Utst * Utst / 2;
      gm  = b * Utst;
      gds = l * beta * Utst * Utst / 2;
    }
    // linear region
    else {
      Ids = b * Vds * (Utst - Vds / 2);
      gm  = b * Vds;
      gds = b * (Utst - Vds) + l * beta * Vds * (Utst - Vds / 2);
    }
    gmb = gm * arg;
  }
  Udsat = pol * MAX (Utst, 0);
  Ids = MOSdir * Ids;
  Uon = pol * Uon;

  // compute autonomic current sources
  IeqBD = Ibd - gbd * Ubd;
  IeqBS = Ibs - gbs * Ubs;

  // exchange controlling nodes if necessary
  SourceControl = (MOSdir > 0) ? (gm + gmb) : 0;
  DrainControl  = (MOSdir < 0) ? (gm + gmb) : 0;
  if (MOSdir > 0) {
    IeqDS = Ids - gm * Ugs - gmb * Ubs - gds * Uds;
  } else {
    IeqDS = Ids - gm * Ugd - gmb * Ubd - gds * Uds;
  }

  setI (NODE_G, 0);
  setI (NODE_D, (+IeqBD - IeqDS) * pol);
  setI (NODE_S, (+IeqBS + IeqDS) * pol);
  setI (NODE_B, (-IeqBD - IeqBS) * pol);

  // apply admittance matrix elements
  setY (NODE_G, NODE_G, 0);
  setY (NODE_G, NODE_D, 0);
  setY (NODE_G, NODE_S, 0);
  setY (NODE_G, NODE_B, 0);
  setY (NODE_D, NODE_G, gm);
  setY (NODE_D, NODE_D, gds + gbd - DrainControl);
  setY (NODE_D, NODE_S, -gds - SourceControl);
  setY (NODE_D, NODE_B, gmb - gbd);
  setY (NODE_S, NODE_G, -gm);
  setY (NODE_S, NODE_D, -gds + DrainControl);
  setY (NODE_S, NODE_S, gbs + gds + SourceControl);
  setY (NODE_S, NODE_B, -gbs - gmb);
  setY (NODE_B, NODE_G, 0);
  setY (NODE_B, NODE_D, -gbd);
  setY (NODE_B, NODE_S, -gbs);
  setY (NODE_B, NODE_B, gbs + gbd);
}

/* Usual and additional state definitions. */

#define qgdState  0 // gate-drain charge state
#define igdState  1 // gate-drain current state
#define vgdState  2 // gate-drain voltage state
#define cgdState  3 // gate-drain capacitance state
#define qgsState  4 // gate-source charge state
#define igsState  5 // gate-source current state
#define vgsState  6 // gate-source voltage state
#define cgsState  7 // gate-source capacitance state
#define qbdState  8 // bulk-drain charge state
#define ibdState  9 // bulk-drain current state
#define qbsState 10 // bulk-source charge state
#define ibsState 11 // bulk-source current state
#define qgbState 12 // gate-bulk charge state
#define igbState 13 // gate-bulk current state
#define vgbState 14 // gate-bulk voltage state
#define cgbState 15 // gate-bulk capacitance state

void mosfet::saveOperatingPoints (void) {
  nr_double_t Vgs, Vgd, Vbs, Vbd;
  Vgd = real (getV (NODE_G) - getV (NODE_D)) * pol;
  Vgs = real (getV (NODE_G) - getV (NODE_S)) * pol;
  Vbs = real (getV (NODE_B) - getV (NODE_S)) * pol;
  Vbd = real (getV (NODE_B) - getV (NODE_D)) * pol;
  setOperatingPoint ("Vgs", Vgs);
  setOperatingPoint ("Vgd", Vgd);
  setOperatingPoint ("Vbs", Vbs);
  setOperatingPoint ("Vbd", Vbd);
  setOperatingPoint ("Vds", Vgs - Vgd);
  setOperatingPoint ("Vgb", Vgs - Vbs);
}

void mosfet::loadOperatingPoints (void) {
  Ugs = getOperatingPoint ("Vgs");
  Ugd = getOperatingPoint ("Vgd");
  Ubs = getOperatingPoint ("Vbs");
  Ubd = getOperatingPoint ("Vbd");
  Uds = getOperatingPoint ("Vds");
  Ugb = getOperatingPoint ("Vgb");
}

void mosfet::calcOperatingPoints (void) {

  // fetch device model parameters
  nr_double_t Cbd0 = getScaledProperty ("Cbd");
  nr_double_t Cbs0 = getScaledProperty ("Cbs");
  nr_double_t Cbds = getPropertyDouble ("Cbds");
  nr_double_t Cbss = getPropertyDouble ("Cbss");
  nr_double_t Cgso = getPropertyDouble ("Cgso");
  nr_double_t Cgdo = getPropertyDouble ("Cgdo");
  nr_double_t Cgbo = getPropertyDouble ("Cgbo");
  nr_double_t Pb   = getScaledProperty ("Pb");
  nr_double_t M    = getPropertyDouble ("Mj");
  nr_double_t Ms   = getPropertyDouble ("Mjsw");
  nr_double_t Fc   = getPropertyDouble ("Fc");
  nr_double_t Tt   = getPropertyDouble ("Tt");
  nr_double_t W    = getPropertyDouble ("W");
  
  nr_double_t Cbs, Cbd, Cgd, Cgb, Cgs;

  // capacitance of bulk-drain diode
  Cbd = gbd * Tt + pnCapacitance (Ubd, Cbd0, Pb, M, Fc) +
    pnCapacitance (Ubd, Cbds, Pb, Ms, Fc);
  Qbd = Ibd * Tt + pnCharge (Ubd, Cbd0, Pb, M, Fc) +
    pnCharge (Ubd, Cbds, Pb, Ms, Fc);

  // capacitance of bulk-source diode
  Cbs = gbs * Tt + pnCapacitance (Ubs, Cbs0, Pb, M, Fc) +
    pnCapacitance (Ubs, Cbss, Pb, Ms, Fc);
  Qbs = Ibs * Tt + pnCharge (Ubs, Cbs0, Pb, M, Fc) +
    pnCharge (Ubs, Cbss, Pb, Ms, Fc);

  // calculate bias-dependent MOS overlap capacitances
  if (MOSdir > 0) {
    fetCapacitanceMeyer (Ugs, Ugd, Uon, Udsat, Phi, Cox, Cgs, Cgd, Cgb);
  } else {
    fetCapacitanceMeyer (Ugd, Ugs, Uon, Udsat, Phi, Cox, Cgd, Cgs, Cgb);
  }

  // charge approximation
  if (transientMode) {
    if (transientMode == 1) {      // by trapezoidal rule
      // gate-source charge
      Qgs = transientChargeTR (qgsState, Cgs, Ugs, Cgso * W);
      // gate-drain charge
      Qgd = transientChargeTR (qgdState, Cgd, Ugd, Cgdo * W);
      // gate-bulk charge
      Qgb = transientChargeTR (qgbState, Cgb, Ugb, Cgbo * Leff);
    }
    else if (transientMode == 2) { // by simpson's rule
      Qgs = transientChargeSR (qgsState, Cgs, Ugs, Cgso * W);
      Qgd = transientChargeSR (qgdState, Cgd, Ugd, Cgdo * W);
      Qgb = transientChargeSR (qgbState, Cgb, Ugb, Cgbo * Leff);
    }
  }
  // usual operating point
  else {
    Cgs += Cgso * W;
    Cgd += Cgdo * W;
    Cgb += Cgbo * Leff;
  }

  // save operating points
  setOperatingPoint ("Id", Ids);
  setOperatingPoint ("gm", gm);
  setOperatingPoint ("gmb", gmb);
  setOperatingPoint ("gds", gds);
  setOperatingPoint ("Vth", Vto);
  setOperatingPoint ("Vdsat", Udsat);
  setOperatingPoint ("gbs", gbs);
  setOperatingPoint ("gbd", gbd);
  setOperatingPoint ("Cbd", Cbd);
  setOperatingPoint ("Cbs", Cbs);
  setOperatingPoint ("Cgs", Cgs);
  setOperatingPoint ("Cgd", Cgd);
  setOperatingPoint ("Cgb", Cgb);
}

void mosfet::initAC (void) {
  allocMatrixMNA ();
}

void mosfet::calcAC (nr_double_t frequency) {
  setMatrixY (calcMatrixY (frequency));
}

void mosfet::calcNoiseAC (nr_double_t frequency) {
  setMatrixN (calcMatrixCy (frequency));
}

void mosfet::initTR (void) {
  setStates (16);
  initDC ();
}

void mosfet::calcTR (nr_double_t) {
  calcDC ();
  transientMode = getPropertyInteger ("capModel");
  saveOperatingPoints ();
  loadOperatingPoints ();
  calcOperatingPoints ();
  transientMode = 0;

  nr_double_t Cgd = getOperatingPoint ("Cgd");
  nr_double_t Cgs = getOperatingPoint ("Cgs");
  nr_double_t Cbd = getOperatingPoint ("Cbd");
  nr_double_t Cbs = getOperatingPoint ("Cbs");
  nr_double_t Cgb = getOperatingPoint ("Cgb");

  Uds = Ugs - Ugd;
  Ugb = Ugs - Ubs;

  transientCapacitance (qbdState, NODE_B, NODE_D, Cbd, Ubd, Qbd);
  transientCapacitance (qbsState, NODE_B, NODE_S, Cbs, Ubs, Qbs);

  // handle Meyer charges and capacitances
  transientCapacitance (qgdState, NODE_G, NODE_D, Cgd, Ugd, Qgd);
  transientCapacitance (qgsState, NODE_G, NODE_S, Cgs, Ugs, Qgs);
  transientCapacitance (qgbState, NODE_G, NODE_B, Cgb, Ugb, Qgb);
}

/* The function uses the trapezoidal rule to compute the current
   capacitance and charge.  The approximation is necessary because the
   Meyer model is a capacitance model and not a charge model. */
nr_double_t mosfet::transientChargeTR (int qstate, nr_double_t& cap,
				       nr_double_t voltage, nr_double_t ccap) {
  int vstate = qstate + 2, cstate = qstate + 3;
  setState (cstate, cap);
  cap = (cap + getState (cstate, 1)) / 2 + ccap;
  setState (vstate, voltage);
  return cap * (voltage - getState (vstate, 1)) + getState (qstate, 1);
}

/* The function uses Simpson's numerical integration rule to compute
   the current capacitance and charge. */
nr_double_t mosfet::transientChargeSR (int qstate, nr_double_t& cap,
				       nr_double_t voltage, nr_double_t ccap) {
  int vstate = qstate + 2, cstate = qstate + 3;
  setState (cstate, cap);
  cap = (cap + 4 * getState (cstate, 1) + getState (cstate, 2)) / 6 + ccap;
  setState (vstate, voltage);
  return cap * (voltage - getState (vstate, 1)) + getState (qstate, 1);
}
