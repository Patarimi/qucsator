/*
 * isolator.cpp - isolator class implementation
 *
 * Copyright (C) 2003, 2004, 2005 Stefan Jahn <stefan@lkcc.org>
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
 * the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.  
 *
 * $Id: isolator.cpp,v 1.14 2005-05-02 06:51:01 raimi Exp $
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "complex.h"
#include "object.h"
#include "node.h"
#include "circuit.h"
#include "constants.h"
#include "component_id.h"
#include "isolator.h"

isolator::isolator () : circuit (2) {
  type = CIR_ISOLATOR;
}

void isolator::initSP (void) {
  nr_double_t z1 = getPropertyDouble ("Z1");
  nr_double_t z2 = getPropertyDouble ("Z2");
  nr_double_t s1 = (z1 - z0) / (z1 + z0);
  nr_double_t s2 = (z2 - z0) / (z2 + z0);
  allocMatrixS ();
  setS (NODE_1, NODE_1, s1);
  setS (NODE_2, NODE_2, s2);
  setS (NODE_1, NODE_2, 0);
  setS (NODE_2, NODE_1, sqrt (1 - s1 * s1) * sqrt (1 - s2 * s2));
}

void isolator::calcNoiseSP (nr_double_t) {
  nr_double_t T = getPropertyDouble ("Temp");
  nr_double_t z1 = getPropertyDouble ("Z1");
  nr_double_t z2 = getPropertyDouble ("Z2");
  nr_double_t r = (z0 - z1) / (z0 + z2);
  nr_double_t f = 4 * z0 / sqr (z1 + z0) * kelvin (T) / T0;
  setN (NODE_1, NODE_1, f * z1);
  setN (NODE_1, NODE_2, f * sqrt (z1 * z2) * r);
  setN (NODE_2, NODE_1, f * sqrt (z1 * z2) * r);
  setN (NODE_2, NODE_2, f * z2 * r * r);
}

void isolator::calcNoiseAC (nr_double_t) {
  nr_double_t T = getPropertyDouble ("Temp");
  nr_double_t z1 = getPropertyDouble ("Z1");
  nr_double_t z2 = getPropertyDouble ("Z2");
  nr_double_t f = 4 * kelvin (T) / T0;
  setN (NODE_1, NODE_1, +f / z1);
  setN (NODE_1, NODE_2, 0);
  setN (NODE_2, NODE_1, -f * 2 / sqrt (z1 * z2));
  setN (NODE_2, NODE_2, +f / z2);
}

void isolator::initDC (void) {
  nr_double_t z1 = getPropertyDouble ("Z1");
  nr_double_t z2 = getPropertyDouble ("Z2");
#if AUGMENTED
  nr_double_t z21 = 2 * sqrt (z1 * z2);
  setVoltageSources (2);
  allocMatrixMNA ();
  setB (NODE_1, VSRC_1, +1.0); setB (NODE_1, VSRC_2, +0.0);
  setB (NODE_2, VSRC_1, +0.0); setB (NODE_2, VSRC_2, +1.0);
  setC (VSRC_1, NODE_1, -1.0); setC (VSRC_1, NODE_2, +0.0);
  setC (VSRC_2, NODE_1, +0.0); setC (VSRC_2, NODE_2, -1.0); 
  setD (VSRC_1, VSRC_1,  +z1); setD (VSRC_2, VSRC_2,  +z2);
  setD (VSRC_1, VSRC_2, +0.0); setD (VSRC_2, VSRC_1, +z21);
  setE (VSRC_1, +0.0); setE (VSRC_2, +0.0);
#else
  setVoltageSources (0);
  allocMatrixMNA ();
  setY (NODE_1, NODE_1, 1 / z1);
  setY (NODE_1, NODE_2, 0);
  setY (NODE_2, NODE_1, -2 / sqrt (z1 * z2));
  setY (NODE_2, NODE_2, 1 / z2);
#endif
}

void isolator::initAC (void) {
  initDC ();
}

void isolator::initTR (void) {
  initDC ();
}
