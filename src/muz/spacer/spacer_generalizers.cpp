/*++
Copyright (c) 2011 Microsoft Corporation

Module Name:

    spacer_generalizers.cpp

Abstract:

    Generalizers of satisfiable states and unsat cores.

Author:

    Nikolaj Bjorner (nbjorner) 2011-11-20.

Revision History:

--*/


#include "spacer_context.h"
#include "spacer_farkas_learner.h"
#include "spacer_generalizers.h"
#include "expr_abstract.h"
#include "var_subst.h"


namespace spacer {


    // ------------------------
    // core_bool_inductive_generalizer

    // main propositional induction generalizer.
    // drop literals one by one from the core and check if the core is still inductive.
    //    
    void core_bool_inductive_generalizer::operator()(model_node& n, expr_ref_vector& core, unsigned& uses_level) {
        if (core.size() <= 1) {
            return;
        }
        ast_manager& m = core.get_manager();
        TRACE("spacer", for (unsigned i = 0; i < core.size(); ++i) { tout << mk_pp(core[i].get(), m) << "\n"; });
        unsigned num_failures = 0, i = 0, old_core_size = core.size();
        ptr_vector<expr> processed;

        while (i < core.size() && 1 < core.size() && (!m_failure_limit || num_failures <= m_failure_limit)) {
            expr_ref lit(m);
            lit = core[i].get();
            core[i] = m.mk_true();            
            if (n.pt().check_inductive(n.level(), core, uses_level)) {
                num_failures = 0;
                for (i = 0; i < core.size() && processed.contains(core[i].get()); ++i);
            }
            else {
                core[i] = lit;
                processed.push_back(lit);
                ++num_failures;
                ++i;
            }
        }
        IF_VERBOSE(2, verbose_stream() << "old size: " << old_core_size << " new size: " << core.size() << "\n";);
        TRACE("spacer", tout << "old size: " << old_core_size << " new size: " << core.size() << "\n";);
    }


    void core_multi_generalizer::operator()(model_node& n, expr_ref_vector& core, 
                                            unsigned& uses_level) {
        UNREACHABLE();
    }

    /**
       \brief Find minimal cores.
       Apply a simple heuristic: find a minimal core, then find minimal cores that exclude at least one
       literal from each of the literals in the minimal cores.
    */
    void core_multi_generalizer::operator()(model_node& n, expr_ref_vector const& core, unsigned uses_level, cores& new_cores) {
        ast_manager& m = core.get_manager();
        expr_ref_vector old_core(m), core0(core);
        unsigned uses_level1 = uses_level;
        m_gen(n, core0, uses_level1);
        new_cores.push_back(std::make_pair(core0, uses_level1));
        obj_hashtable<expr> core_exprs, core1_exprs;
        datalog::set_union(core_exprs, core0);
        for (unsigned i = 0; i < old_core.size(); ++i) {
            expr* lit = old_core[i].get();             
            if (core_exprs.contains(lit)) {
                expr_ref_vector core1(old_core);
                core1[i] = core1.back();
                core1.pop_back();
                uses_level1 = uses_level;
                m_gen(n, core1, uses_level1);
                SASSERT(core1.size() <= old_core.size());
                if (core1.size() < old_core.size()) {
                    new_cores.push_back(std::make_pair(core1, uses_level1));
                    core1_exprs.reset();
                    datalog::set_union(core1_exprs, core1);
                    datalog::set_intersection(core_exprs, core1_exprs);
                }
            }
        }
    }

    // ------------------------
    // core_farkas_generalizer 

    // 
    // for each disjunct of core:
    //     weaken predecessor.
    //    

    core_farkas_generalizer::core_farkas_generalizer(context& ctx, ast_manager& m, smt_params& p):
        core_generalizer(ctx), 
        m_farkas_learner(p, m) 
    {}
    
    void core_farkas_generalizer::operator()(model_node& n, expr_ref_vector& core, 
                                             unsigned& uses_level) {
        ast_manager& m  = n.pt().get_ast_manager();
        manager& pm = n.pt().get_manager();
        if (core.empty()) return;
        expr_ref A(m), B(pm.mk_and(core)), C(m);
        expr_ref_vector Bs(m);
        pm.get_or(B, Bs);
        A = n.pt().get_propagation_formula(m_ctx.get_pred_transformers(), n.level());

        bool change = false;
        for (unsigned i = 0; i < Bs.size(); ++i) {
            expr_ref_vector lemmas(m);
            C = Bs[i].get();
            if (m_farkas_learner.get_lemma_guesses(A, B, lemmas)) {
                TRACE("spacer", 
                      tout << "Old core:\n" << mk_pp(B, m) << "\n";
                      tout << "New core:\n" << mk_pp(pm.mk_and(lemmas), m) << "\n";);            
                Bs[i] = pm.mk_and(lemmas);
                change = true;
            }
        }
        if (change) {
            C = pm.mk_or(Bs);
            TRACE("spacer", tout << "prop:\n" << mk_pp(A,m) << "\ngen:" << mk_pp(B, m) << "\nto: " << mk_pp(C, m) << "\n";);
            core.reset();
            qe::flatten_and(C, core);    
            uses_level = n.level ();
        }    
    }

    void core_farkas_generalizer::collect_statistics(statistics& st) const {
        m_farkas_learner.collect_statistics(st);
    }


    // ---------------------------------
    // core_arith_inductive_generalizer 
    // NB. this is trying out some ideas for generalization in
    // an ad hoc specialized way. arith_inductive_generalizer should
    // not be used by default. It is a place-holder for a general purpose
    // extrapolator of a lattice basis.

    core_arith_inductive_generalizer::core_arith_inductive_generalizer(context& ctx):
        core_generalizer(ctx), 
        m(ctx.get_ast_manager()),
        a(m),
        m_refs(m) {}

    void core_arith_inductive_generalizer::operator()(model_node& n, 
                                                      expr_ref_vector& core, 
                                                      unsigned& uses_level) {
        if (core.size() <= 1) {
            return;
        }
        reset();
        expr_ref e(m), t1(m), t2(m), t3(m);
        rational r;
        
        TRACE("spacer", for (unsigned i = 0; i < core.size(); ++i) { tout << mk_pp(core[i].get(), m) << "\n"; });

        svector<eq> eqs;
        get_eqs(core, eqs);

        if (eqs.empty()) {
            return;
        }

        expr_ref_vector new_core(m);
        new_core.append(core);

        for (unsigned eq = 0; eq < eqs.size(); ++eq) {
            rational r = eqs[eq].m_value;
            expr* x = eqs[eq].m_term;
            unsigned k = eqs[eq].m_i;
            unsigned l = eqs[eq].m_j;

            new_core[l] = m.mk_true();
            new_core[k] = m.mk_true();
            
            for (unsigned i = 0; i < new_core.size(); ++i) {
                if (substitute_alias(r, x, new_core[i].get(), e)) {
                    new_core[i] = e;
                }
            }
            if (abs(r) >= rational(2) && a.is_int(x)) {
                new_core[k] = m.mk_eq(a.mk_mod(x, a.mk_numeral(rational(2), true)), a.mk_numeral(rational(0), true));
                new_core[l] = a.mk_le(x, a.mk_numeral(rational(0), true));
            }
        }

        bool inductive = n.pt().check_inductive(n.level(), new_core, uses_level);

        IF_VERBOSE(1, 
                   verbose_stream() << (inductive?"":"non") << "inductive\n";
                   verbose_stream() << "old\n";
                   for (unsigned j = 0; j < core.size(); ++j) { 
                       verbose_stream() << mk_pp(core[j].get(), m) << "\n"; 
                   }
                   verbose_stream() << "new\n";
                   for (unsigned j = 0; j < new_core.size(); ++j) { 
                       verbose_stream() << mk_pp(new_core[j].get(), m) << "\n"; 
                   });
        
        if (inductive) {
            core.reset();
            core.append(new_core);
        }
    }    

    void core_arith_inductive_generalizer::insert_bound(bool is_lower, expr* x, rational const& r, unsigned i) {
        if (r.is_neg()) {
            expr_ref e(m);
            e = a.mk_uminus(x);
            m_refs.push_back(e);
            x = e;
            is_lower = !is_lower;
        }

        vector<term_loc_t> bound;
        bound.push_back(std::make_pair(x, i));
        if (is_lower) {
            m_lb.insert(abs(r), bound);
        }
        else {
            m_ub.insert(abs(r), bound);
        }
    }

    void core_arith_inductive_generalizer::reset() {
        m_refs.reset();
        m_lb.reset();
        m_ub.reset();
    }

    void core_arith_inductive_generalizer::get_eqs(expr_ref_vector const& core, svector<eq>& eqs) {
        expr* e1, *x, *y;
        expr_ref e(m);
        rational r;

        for (unsigned i = 0; i < core.size(); ++i) {
            e = core[i];
            if (m.is_not(e, e1) && a.is_le(e1, x, y) && a.is_numeral(y, r) && a.is_int(x)) {
                // not (<= x r) <=> x >= r + 1
                insert_bound(true, x, r + rational(1), i);
            }
            else if (m.is_not(e, e1) && a.is_ge(e1, x, y) && a.is_numeral(y, r) && a.is_int(x)) {
                // not (>= x r) <=> x <= r - 1
                insert_bound(false, x, r - rational(1), i);
            }
            else if (a.is_le(e, x, y) && a.is_numeral(y, r)) {
                insert_bound(false, x, r, i);
            }
            else if (a.is_ge(e, x, y) && a.is_numeral(y, r)) {
                insert_bound(true, x, r, i);
            }
        }
        bounds_t::iterator it = m_lb.begin(), end = m_lb.end();
        for (; it != end; ++it) {
            rational r = it->m_key;
            vector<term_loc_t> & terms1 = it->m_value;
            vector<term_loc_t> terms2;
            if (r >= rational(2) && m_ub.find(r, terms2)) {
                for (unsigned i = 0; i < terms1.size(); ++i) {
                    bool done = false;
                    for (unsigned j = 0; !done && j < terms2.size(); ++j) {
                        expr* t1 = terms1[i].first;
                        expr* t2 = terms2[j].first;
                        if (t1 == t2) {
                            eqs.push_back(eq(t1, r, terms1[i].second, terms2[j].second));
                            done = true;
                        }
                        else {
                            e = m.mk_eq(t1, t2);
                            th_rewriter rw(m);
                            rw(e);
                            if (m.is_true(e)) {
                                eqs.push_back(eq(t1, r, terms1[i].second, terms2[j].second));
                                done = true;                                
                            }
                        }
                    }
                }
            }
        }
    }

    bool core_arith_inductive_generalizer::substitute_alias(rational const& r, expr* x, expr* e, expr_ref& result) {
        rational r2;
        expr* y, *z, *e1;
        if (m.is_not(e, e1) && substitute_alias(r, x, e1, result)) {
            result = m.mk_not(result);
            return true;
        }
        if (a.is_le(e, y, z) && a.is_numeral(z, r2)) {
            if (r == r2) {
                result = a.mk_le(y, x);
                return true;
            }
            if (r == r2 + rational(1)) {
                result = a.mk_lt(y, x);
                return true;
            }
            if (r == r2 - rational(1)) {
                result = a.mk_le(y, a.mk_sub(x, a.mk_numeral(rational(1), a.is_int(x))));
                return true;
            }
            
        }
        if (a.is_ge(e, y, z) && a.is_numeral(z, r2)) {
            if (r == r2) {
                result = a.mk_ge(y, x);
                return true;
            }
            if (r2 == r + rational(1)) {
                result = a.mk_gt(y, x);
                return true;
            }
            if (r2 == r - rational(1)) {
                result = a.mk_ge(y, a.mk_sub(x, a.mk_numeral(rational(1), a.is_int(x))));
                return true;
            }
        }
        return false;
    }


    // 
    //     < F, phi, i + 1> 
    //             |
    //      < G, psi, i >
    // 
    // where:
    //
    //  p(x) <- F(x,y,p,q)
    //  q(x) <- G(x,y)
    // 
    // Hyp:
    //  Q_k(x) => phi(x)           j <= k <= i
    //  Q_k(x) => R_k(x)           j <= k <= i + 1
    //  Q_k(x) <=> Trans(Q_{k-1})  j <  k <= i + 1
    // Conclusion:
    //  Q_{i+1}(x) => phi(x)
    //
    class core_induction_generalizer::imp {
        context&     m_ctx;
        manager& pm;
        ast_manager& m;

        //
        //  Create predicate Q_level
        // 
        func_decl_ref mk_pred(unsigned level, func_decl* f) {
            func_decl_ref result(m);
            std::ostringstream name;
            name << f->get_name() << "_" << level;
            symbol sname(name.str().c_str());
            result = m.mk_func_decl(sname, f->get_arity(), f->get_domain(), f->get_range());
            return result;
        }

        // 
        // Create formula exists y . z . F[Q_{level-1}, x, y, z]
        //
        expr_ref mk_transition_rule(
            expr_ref_vector const& reps, 
            unsigned level, 
            datalog::rule const& rule) 
        {
            expr_ref_vector conj(m), sub(m);
            expr_ref result(m);
            svector<symbol> names;
            unsigned ut_size = rule.get_uninterpreted_tail_size();
            unsigned t_size = rule.get_tail_size();              
            if (0 == level && 0 < ut_size) {
                result = m.mk_false();
                return result;
            }
            app* atom = rule.get_head();
            SASSERT(atom->get_num_args() == reps.size());
            
            for (unsigned i = 0; i < reps.size(); ++i) {
                expr* arg = atom->get_arg(i);
                if (is_var(arg)) {
                    unsigned idx = to_var(arg)->get_idx();
                    if (idx >= sub.size()) sub.resize(idx+1);
                    if (sub[idx].get()) {
                        conj.push_back(m.mk_eq(sub[idx].get(), reps[i]));
                    }
                    else {
                        sub[idx] = reps[i];
                    }
                }
                else {
                    conj.push_back(m.mk_eq(arg, reps[i]));
                }
            }
            for (unsigned i = 0; 0 < level && i < ut_size; i++) {
                app* atom = rule.get_tail(i);
                func_decl* head = atom->get_decl();
                func_decl_ref fn = mk_pred(level-1, head);
                conj.push_back(m.mk_app(fn, atom->get_num_args(), atom->get_args()));
            }                        
            for (unsigned i = ut_size; i < t_size; i++) {
                conj.push_back(rule.get_tail(i));
            }         
            result = pm.mk_and(conj);
            if (!sub.empty()) {
                expr_ref tmp = result;
                var_subst(m, false)(tmp, sub.size(), sub.c_ptr(), result);
            }
            expr_free_vars fv;
            fv (result);
            fv.set_default_sort (m.mk_bool_sort ());
            for (unsigned i = 0; i < fv.size(); ++i) {
                names.push_back(symbol(fv.size() - i - 1));
            }
            if (!fv.empty()) {
                fv.reverse();
                result = m.mk_exists(fv.size(), fv.c_ptr(), names.c_ptr(), result); 
            }            
            return result;
        }

        expr_ref bind_head(expr_ref_vector const& reps, expr* fml) {
            expr_ref result(m);
            expr_abstract(m, 0, reps.size(), reps.c_ptr(), fml, result);
            ptr_vector<sort> sorts;
            svector<symbol>  names;
            unsigned sz = reps.size();
            for (unsigned i = 0; i < sz; ++i) {
                sorts.push_back(m.get_sort(reps[sz-i-1]));
                names.push_back(symbol(sz-i-1));
            }
            if (sz > 0) {
                result = m.mk_forall(sorts.size(), sorts.c_ptr(), names.c_ptr(), result);
            }
            return result;
        }

        expr_ref_vector mk_reps(pred_transformer& pt) {
            expr_ref_vector reps(m);
            expr_ref rep(m);
            for (unsigned i = 0; i < pt.head()->get_arity(); ++i) {
                rep = m.mk_const(pm.o2n(pt.sig(i), 0));
                reps.push_back(rep);
            }
            return reps;
        }

        //
        // extract transition axiom:
        // 
        //  forall x . p_lvl(x) <=> exists y z . F[p_{lvl-1}(y), q_{lvl-1}(z), x]
        //
        expr_ref mk_transition_axiom(pred_transformer& pt, unsigned level) {
            expr_ref fml(m.mk_false(), m), tr(m);
            expr_ref_vector reps = mk_reps(pt);
            ptr_vector<datalog::rule> const& rules = pt.rules();
            for (unsigned i = 0; i < rules.size(); ++i) {
                tr = mk_transition_rule(reps, level, *rules[i]);
                fml = (i == 0)?tr.get():m.mk_or(fml, tr);
            }
            func_decl_ref fn = mk_pred(level, pt.head());
            fml = m.mk_iff(m.mk_app(fn, reps.size(), reps.c_ptr()), fml);
            fml = bind_head(reps, fml);
            return fml;
        }

        // 
        // Create implication:
        //  Q_level(x) => phi(x)    
        //
        expr_ref mk_predicate_property(unsigned level, pred_transformer& pt, expr* phi) {
            expr_ref_vector reps = mk_reps(pt);
            func_decl_ref fn = mk_pred(level, pt.head());
            expr_ref fml(m);
            fml = m.mk_implies(m.mk_app(fn, reps.size(), reps.c_ptr()), phi);
            fml = bind_head(reps, fml);
            return fml;            
        }



    public:
        imp(context& ctx): m_ctx(ctx), pm(ctx.get_manager()), m(ctx.get_ast_manager()) {}

        // 
        // not exists y . F(x,y)
        // 
        expr_ref mk_blocked_transition(pred_transformer& pt, unsigned level) {
            SASSERT(level > 0);
            expr_ref fml(m.mk_true(), m);
            expr_ref_vector reps = mk_reps(pt), fmls(m);
            ptr_vector<datalog::rule> const& rules = pt.rules();
            for (unsigned i = 0; i < rules.size(); ++i) {
                fmls.push_back(m.mk_not(mk_transition_rule(reps, level, *rules[i])));           
            }
            fml = pm.mk_and(fmls);
            TRACE("spacer", tout << mk_pp(fml, m) << "\n";);
            return fml;
        }

        expr_ref mk_induction_goal(pred_transformer& pt, unsigned level, unsigned depth) {
            SASSERT(level >= depth);
            expr_ref_vector conjs(m);
            ptr_vector<pred_transformer> pts;
            unsigned_vector levels;
            // negated goal
            expr_ref phi = mk_blocked_transition(pt, level);
            conjs.push_back(m.mk_not(mk_predicate_property(level, pt, phi)));
            pts.push_back(&pt);
            levels.push_back(level);
            // Add I.H. 
            for (unsigned lvl = level-depth; lvl < level; ++lvl) {
                if (lvl > 0) {
                    expr_ref psi = mk_blocked_transition(pt, lvl);
                    conjs.push_back(mk_predicate_property(lvl, pt, psi));
                    pts.push_back(&pt);
                    levels.push_back(lvl);
                }
            }
            // Transitions:
            for (unsigned qhead = 0; qhead < pts.size(); ++qhead) {
                pred_transformer& qt = *pts[qhead];
                unsigned lvl = levels[qhead];

                // Add transition definition and properties at level.
                conjs.push_back(mk_transition_axiom(qt, lvl));
                conjs.push_back(mk_predicate_property(lvl, qt, qt.get_formulas(lvl, true)));
                
                // Enqueue additional hypotheses
                ptr_vector<datalog::rule> const& rules = qt.rules();
                if (lvl + depth < level || lvl == 0) {
                    continue;
                }
                for (unsigned i = 0; i < rules.size(); ++i) {
                    datalog::rule& r = *rules[i];
                    unsigned ut_size = r.get_uninterpreted_tail_size();
                    for (unsigned j = 0; j < ut_size; ++j) {
                        func_decl* f = r.get_tail(j)->get_decl();
                        pred_transformer* rt = m_ctx.get_pred_transformers().find(f);
                        bool found = false;
                        for (unsigned k = 0; !found && k < levels.size(); ++k) {
                            found = (rt == pts[k] && levels[k] + 1 == lvl);
                        }
                        if (!found) {
                            levels.push_back(lvl-1);
                            pts.push_back(rt);
                        }
                    }
                }                
            }

            expr_ref result = pm.mk_and(conjs);
            TRACE("spacer", tout << mk_pp(result, m) << "\n";);
            return result;
        }
    };

    //
    // Instantiate Peano induction schema.
    // 
    void core_induction_generalizer::operator()(model_node& n, 
                                                expr_ref_vector& core, 
                                                unsigned& uses_level) {
        model_node* p = n.parent();
        if (p == 0) {
            return;
        }
        unsigned depth = 2;
        imp imp(m_ctx);
        ast_manager& m = core.get_manager();
        expr_ref goal = imp.mk_induction_goal(p->pt(), p->level(), depth);
        smt::kernel ctx(m, m_ctx.get_fparams(), m_ctx.get_params().p);
        ctx.assert_expr(goal);
        lbool r = ctx.check();
        TRACE("spacer", tout << r << "\n";
                     for (unsigned i = 0; i < core.size(); ++i) {
                         tout << mk_pp(core[i].get(), m) << "\n";
                     });
        if (r == l_false) {
            core.reset();
            expr_ref phi = imp.mk_blocked_transition(p->pt(), p->level());
            core.push_back(m.mk_not(phi));
            uses_level = n.level ();
        }
    }
  
  namespace
  {
    class collect_array_proc 
    {
      array_util m_au;
      func_decl_set &m_symbs;
      sort *m_sort;
    public:
      collect_array_proc (ast_manager &m, func_decl_set& s) : 
        m_au (m), m_symbs (s), m_sort(NULL) {}
      
      void operator() (app* a)
      {
        if (a->get_family_id () == null_family_id && m_au.is_array (a))
        {
          if (m_sort && m_sort != get_sort (a)) return;
          if (!m_sort) m_sort = get_sort (a);
          m_symbs.insert (a->get_decl ());
        }
      }
      void operator() (var*) {}
      void operator() (quantifier*) {}
    };
  }
  
  void core_array_eq_generalizer::operator() 
    (model_node &n, expr_ref_vector& core, unsigned &uses_level)
  {
    TRACE ("core_array_eq", tout << "Looking for equalities\n";);
    
    // -- find array constants
    ast_manager &m = m_ctx.get_ast_manager ();
    manager &pm = m_ctx.get_manager ();
    
    expr_ref v(m);
    v = pm.mk_and (core);
    func_decl_set symb;
    collect_array_proc cap (m, symb);
    for_each_expr (cap, v);
    
    TRACE ("core_array_eq", 
           tout << "found " << symb.size () << " array variables in: \n"
           << mk_pp (v, m) << "\n";);
    
    // too few constants
    if (symb.size () <= 1) return;
    // too many constants, skip this
    if (symb.size () >= 8) return;
    
    
    // -- for every pair of variables, try an equality
    typedef func_decl_set::iterator iterator;
    ptr_vector<func_decl> vsymbs;
    for (iterator it = symb.begin (), end = symb.end (); 
         it != end; ++it)
      vsymbs.push_back (*it);
    
    expr_ref_vector eqs (m);
    
    for (unsigned i = 0, sz = vsymbs.size (); i < sz; ++i)
      for (unsigned j = i + 1; j < sz; ++j)
        eqs.push_back (m.mk_eq (m.mk_const (vsymbs.get (i)), m.mk_const (vsymbs.get (j))));
    
    smt::kernel solver (m, m_ctx.get_fparams (), m_ctx.get_params ().p);
    expr_ref_vector lits (m);
    for (unsigned i = 0, core_sz = core.size (); i < core_sz; ++i)
    {
      SASSERT (lits.size () == i);
      solver.push ();
      solver.assert_expr (core.get (i));
      for (unsigned j = 0, eqs_sz = eqs.size (); j < eqs_sz; ++j)
      {
        solver.push ();
        solver.assert_expr (eqs.get (j));
        lbool res = solver.check ();
        solver.pop (1);
        
        if (res == l_false)
        {  
          TRACE ("core_array_eq", 
                 tout << "strengthened " << mk_pp (core.get (i), m)
                 << " with " << mk_pp (m.mk_not (eqs.get (j)), m) << "\n";);
          lits.push_back (m.mk_not (eqs.get (j)));
          break;
        }
      }
      solver.pop (1);
      if (lits.size () == i) lits.push_back (core.get (i));
    }
    
    /**
       HACK: if the first 3 arguments of pt are boolean, assume
       they correspond to SeaHorn encoding and condition the equality on them.
    */
    // pred_transformer &pt = n.pt ();
    // if (pt.sig_size () >= 3 &&
    //     m.is_bool (pt.sig (0)->get_range ()) &&
    //     m.is_bool (pt.sig (1)->get_range ()) &&
    //     m.is_bool (pt.sig (2)->get_range ()))
    // {
    //   lits.push_back (m.mk_const (pm.o2n(pt.sig (0), 0)));
    //   lits.push_back (m.mk_not (m.mk_const (pm.o2n(pt.sig (1), 0))));
    //   lits.push_back (m.mk_not (m.mk_const (pm.o2n(pt.sig (2), 0))));
    // }
          
    TRACE ("core_array_eq", tout << "new possible core " 
           << mk_pp (pm.mk_and (lits), m) << "\n";);
    
          
    // -- check if it is consistent with the transition relation
    unsigned uses_level1;
    if (n.pt ().check_inductive (n.level (), lits, uses_level1))
    {
      TRACE ("core_array_eq", tout << "Inductive!\n";);
      core.reset ();
      core.append (lits);
      uses_level = uses_level1;
      return;
    }
    else
    { TRACE ("core_array_eq", tout << "Not-Inductive!\n";);}
  }
};
