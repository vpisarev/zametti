#ifndef MICROTEX_MACRO_TYPES_H
#define MICROTEX_MACRO_TYPES_H

#include "macro/macro_decl.h"

namespace microtex {

inline sptr<Atom> _math_type(Parser& tp, Args& args, AtomType type) {
  return sptrOf<TypedAtom>(type, type, Formula(tp, args[1], false)._root);
}

inline macro(mathop) {
  auto a = _math_type(tp, args, AtomType::bigOperator);
  // ПАТЧ zametti_math: у TeX \mathop даёт \displaylimits — пределы под и над
  // оператором в выключной формуле и сбоку в строке. Это LimitsType::normal.
  // Было noLimits, из-за чего \lim в выключной формуле ставил предел сбоку.
  // Что задумано именно так, видно по определениям: у \sin и \log стоит
  // явный \nolimits, а у \lim и \limsup — нет.
  a->_limitsType = LimitsType::normal;
  return a;
}

inline macro(mathpunct) {
  return _math_type(tp, args, AtomType::punctuation);
}

inline macro(mathord) {
  return _math_type(tp, args, AtomType::ordinary);
}

inline macro(mathrel) {
  return _math_type(tp, args, AtomType::relation);
}

inline macro(mathinner) {
  return _math_type(tp, args, AtomType::inner);
}

inline macro(mathbin) {
  return _math_type(tp, args, AtomType::binaryOperator);
}

inline macro(mathopen) {
  return _math_type(tp, args, AtomType::opening);
}

inline macro(mathclose) {
  return _math_type(tp, args, AtomType::closing);
}

}  // namespace microtex

#endif  // MICROTEX_MACRO_TYPES_H
