/*
  Transport PK

  Copyright 2010-201x held jointly by LANS/LANL, LBNL, and PNNL. 
  Amanzi is released under the three-clause BSD License. 
  The terms of use and "as is" disclaimer for this license are 
  provided in the top-level COPYRIGHT file.

  Author: Daniil Svyatskiy (dasvyat@lanl.gov)
*/

#include "Transport_PK_ATS.hh"

namespace Amanzi {
namespace Transport {

RegisteredPKFactory<Transport_PK_ATS> Transport_PK_ATS::reg_("transport ats");

}  // namespace Transport
}  // namespace Amanzi
