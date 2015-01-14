/*
Author: Ethan Coon

Painter's permafrost model with freezing point depression, smoothed to make life easier

 */

#ifndef AMANZI_FLOWRELATIONS_WRM_FPD_SMOOTHED_PERMAFROST_MODEL_
#define AMANZI_FLOWRELATIONS_WRM_FPD_SMOOTHED_PERMAFROST_MODEL_

#include "wrm_permafrost_model.hh"
#include "wrm_permafrost_factory.hh"

namespace Amanzi {
namespace Flow {
namespace FlowRelations {

class WRM;

class WRMFPDSmoothedPermafrostModel : public WRMPermafrostModel {

 public:
  explicit
  WRMFPDSmoothedPermafrostModel(Teuchos::ParameterList& plist) :
      WRMPermafrostModel(plist) {}

  // required methods from the base class
  // sats[0] = sg, sats[1] = sl, sats[2] = si
  virtual bool freezing(double T, double pc_liq, double pc_ice) { 
    return pc_liq <= 0. ? T < 273.15 : pc_liq < pc_ice;
  }

  virtual void saturations(double pc_liq, double pc_ice, double T, double (&sats)[3]);
  virtual void dsaturations_dpc_liq(double pc_liq, double pc_ice, double T, double (&dsats)[3]);
  virtual void dsaturations_dpc_ice(double pc_liq, double pc_ice, double T, double (&dsats)[3]);
  virtual void dsaturations_dtemperature(double pc_liq, double pc_ice, double T, double (&dsats)[3]);

 protected:
  double deriv_regularization_;

 private:
  // factory registration
  static Utils::RegisteredFactory<WRMPermafrostModel,WRMFPDSmoothedPermafrostModel> factory_;

};


} //namespace
} //namespace
} //namespace

#endif
