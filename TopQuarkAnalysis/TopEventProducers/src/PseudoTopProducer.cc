#include "TopQuarkAnalysis/TopEventProducers/interface/PseudoTopProducer.h"

#include "CommonTools/Utils/interface/PtComparator.h"

#include "RecoJets/JetProducers/interface/JetSpecific.h"
#include "fastjet/ClusterSequence.hh"

using namespace std;
using namespace edm;
using namespace reco;

PseudoTopProducer::PseudoTopProducer(const edm::ParameterSet& pset):
  leptonMinPt_(pset.getParameter<double>("leptonMinPt")),
  leptonMaxEta_(pset.getParameter<double>("leptonMaxEta")),
  jetMinPt_(pset.getParameter<double>("jetMinPt")),
  jetMaxEta_(pset.getParameter<double>("jetMaxEta")),
  wMass_(pset.getParameter<double>("wMass")),
  tMass_(pset.getParameter<double>("tMass"))
{
  finalStateToken_ = consumes<edm::View<reco::Candidate> >(pset.getParameter<edm::InputTag>("finalStates"));
  genParticleToken_ = consumes<edm::View<reco::Candidate> >(pset.getParameter<edm::InputTag>("genParticles"));

  const double leptonConeSize = pset.getParameter<double>("leptonConeSize");
  const double jetConeSize = pset.getParameter<double>("jetConeSize");
  fjLepDef_ = std::shared_ptr<JetDef>(new JetDef(fastjet::antikt_algorithm, leptonConeSize));
  fjJetDef_ = std::shared_ptr<JetDef>(new JetDef(fastjet::antikt_algorithm, jetConeSize));

  genVertex_ = reco::Particle::Point(0,0,0);

  produces<reco::GenParticleCollection>("neutrinos");
  produces<reco::GenJetCollection>("leptons");
  produces<reco::GenJetCollection>("jets");

  produces<reco::GenParticleCollection>();

}

void PseudoTopProducer::produce(edm::Event& event, const edm::EventSetup& eventSetup)
{
  edm::Handle<edm::View<reco::Candidate> > finalStateHandle;
  event.getByToken(finalStateToken_, finalStateHandle);

  edm::Handle<edm::View<reco::Candidate> > genParticleHandle;
  event.getByToken(genParticleToken_, genParticleHandle);

  std::auto_ptr<reco::GenParticleCollection> neutrinos(new reco::GenParticleCollection);
  std::auto_ptr<reco::GenJetCollection> leptons(new reco::GenJetCollection);
  std::auto_ptr<reco::GenJetCollection> jets(new reco::GenJetCollection);
  auto neutrinosRefHandle = event.getRefBeforePut<reco::GenParticleCollection>("neutrinos");
  auto leptonsRefHandle = event.getRefBeforePut<reco::GenJetCollection>("leptons");
  auto jetsRefHandle = event.getRefBeforePut<reco::GenJetCollection>("jets");

  std::auto_ptr<reco::GenParticleCollection> pseudoTop(new reco::GenParticleCollection);
  auto pseudoTopRefHandle = event.getRefBeforePut<reco::GenParticleCollection>();

  // Collect unstable B-hadrons
  std::set<size_t> bHadronIdxs;
  for ( size_t i=0, n=genParticleHandle->size(); i<n; ++i ) {
    const reco::Candidate& p = genParticleHandle->at(i);
    const int status = p.status();
    if ( status == 1 ) continue;

    // Collect B-hadrons, to be used in b tagging
    if ( isBHadron(&p) ) bHadronIdxs.insert(i);
  }

  // Collect stable leptons and neutrinos
  size_t nStables = 0;
  std::vector<size_t> leptonIdxs;
  for ( size_t i=0, n=finalStateHandle->size(); i<n; ++i ) {
    const reco::Candidate& p = finalStateHandle->at(i);
    const int absPdgId = abs(p.pdgId());
    if ( p.status() != 1 ) continue;

    ++nStables;
    if ( p.numberOfMothers() == 0 ) continue; // Skip orphans (if exists)
    if ( p.mother()->status() == 4 ) continue; // Treat particle as hadronic if directly from the incident beam (protect orphans in MINIAOD)
    if ( isFromHadron(&p) ) continue;
    switch ( absPdgId ) {
      case 11: case 13: // Leptons
      case 22: // Photons
        leptonIdxs.push_back(i);
        break;
      case 12: case 14: case 16:
        neutrinos->push_back(reco::GenParticle(p.charge(), p.p4(), p.vertex(), p.pdgId(), p.status(), true));
        break;
    }
  }

  // Sort neutrinos by pT.
  std::sort(neutrinos->begin(), neutrinos->end(), GreaterByPt<reco::Candidate>());

  // Make dressed leptons with anti-kt(0.1) algorithm
  //// Prepare input particle list
  std::vector<fastjet::PseudoJet> fjLepInputs;
  fjLepInputs.reserve(leptonIdxs.size());
  for ( auto index : leptonIdxs ) {
    const reco::Candidate& p = finalStateHandle->at(index);
    if ( std::isnan(p.pt()) or p.pt() <= 0 ) continue;

    fjLepInputs.push_back(fastjet::PseudoJet(p.px(), p.py(), p.pz(), p.energy()));
    fjLepInputs.back().set_user_index(index);
  }

  //// Run the jet algorithm
  fastjet::ClusterSequence fjLepClusterSeq(fjLepInputs, *fjLepDef_);
  std::vector<fastjet::PseudoJet> fjLepJets = fastjet::sorted_by_pt(fjLepClusterSeq.inclusive_jets(leptonMinPt_));

  //// Build dressed lepton objects from the FJ output
  leptons->reserve(fjLepJets.size());
  std::set<size_t> lepDauIdxs; // keep lepton constituents to remove from GenJet construction
  for ( auto& fjJet : fjLepJets ) {
    if ( abs(fjJet.eta()) > leptonMaxEta_ ) continue;

    // Get jet constituents from fastJet
    const std::vector<fastjet::PseudoJet> fjConstituents = fastjet::sorted_by_pt(fjJet.constituents());
    // Convert to CandidatePtr
    std::vector<reco::CandidatePtr> constituents;
    reco::CandidatePtr lepCand;
    for ( auto& fjConstituent : fjConstituents ) {
      const size_t index = fjConstituent.user_index();
      reco::CandidatePtr cand = finalStateHandle->ptrAt(index);
      const int absPdgId = abs(cand->pdgId());
      if ( absPdgId == 11 or absPdgId == 13 ) {
        if ( lepCand.isNonnull() and lepCand->pt() > cand->pt() ) continue; // Choose one with highest pt
        lepCand = cand;
      }
      constituents.push_back(cand);
    }
    if ( lepCand.isNull() ) continue;

    const LorentzVector jetP4(fjJet.px(), fjJet.py(), fjJet.pz(), fjJet.E());
    reco::GenJet lepJet;
    reco::writeSpecific(lepJet, jetP4, genVertex_, constituents, eventSetup);

    lepJet.setPdgId(lepCand->pdgId());
    lepJet.setCharge(lepCand->charge());

    const double jetArea = fjJet.has_area() ? fjJet.area() : 0;
    lepJet.setJetArea(jetArea);

    leptons->push_back(lepJet);

    // Keep constituent indices to be used in the next step.
    for ( auto& fjConstituent : fjConstituents ) {
      lepDauIdxs.insert(fjConstituent.user_index());
    }
  }

  // Now proceed to jets.
  // Jets: anti-kt excluding the e, mu, nu, and photons in selected leptons.
  //// Prepare input particle list. Remove particles used in lepton clusters, neutrinos
  std::vector<fastjet::PseudoJet> fjJetInputs;
  fjJetInputs.reserve(nStables);
  for ( size_t i=0, n=finalStateHandle->size(); i<n; ++i ) {
    const reco::Candidate& p = finalStateHandle->at(i);
    if ( p.status() != 1 ) continue;
    if ( std::isnan(p.pt()) or p.pt() <= 0 ) continue;

    const int absId = std::abs(p.pdgId());
    if ( absId == 12 or absId == 14 or absId == 16 ) continue;
    if ( lepDauIdxs.find(i) != lepDauIdxs.end() ) continue;

    fjJetInputs.push_back(fastjet::PseudoJet(p.px(), p.py(), p.pz(), p.energy()));
    fjJetInputs.back().set_user_index(i);
  }
  //// Also don't forget to put B hadrons
  for ( auto index : bHadronIdxs ) {
    const reco::Candidate& p = genParticleHandle->at(index);
    if ( std::isnan(p.pt()) or p.pt() <= 0 ) continue;

    const double scale = 1e-20/p.p();
    fjJetInputs.push_back(fastjet::PseudoJet(p.px()*scale, p.py()*scale, p.pz()*scale, p.energy()*scale));
    fjJetInputs.back().set_user_index(index);
  }

  //// Run the jet algorithm
  fastjet::ClusterSequence fjJetClusterSeq(fjJetInputs, *fjJetDef_);
  std::vector<fastjet::PseudoJet> fjJets = fastjet::sorted_by_pt(fjJetClusterSeq.inclusive_jets(jetMinPt_));

  /// Build jets
  jets->reserve(fjJets.size());
  std::vector<size_t> bjetIdxs, ljetIdxs;
  for ( auto& fjJet : fjJets ) {
    if ( abs(fjJet.eta()) > jetMaxEta_ ) continue;

    // Get jet constituents from fastJet
    const std::vector<fastjet::PseudoJet> fjConstituents = fastjet::sorted_by_pt(fjJet.constituents());
    // Convert to CandidatePtr
    std::vector<reco::CandidatePtr> constituents;
    bool hasBHadron = false;
    for ( size_t j=0, m=fjConstituents.size(); j<m; ++j ) {
      const size_t index = fjConstituents[j].user_index();
      if ( bHadronIdxs.find(index) != bHadronIdxs.end() ) hasBHadron = true;
      reco::CandidatePtr cand = finalStateHandle->ptrAt(index);
      constituents.push_back(cand);
    }

    const LorentzVector jetP4(fjJet.px(), fjJet.py(), fjJet.pz(), fjJet.E());
    reco::GenJet genJet;
    reco::writeSpecific(genJet, jetP4, genVertex_, constituents, eventSetup);

    const double jetArea = fjJet.has_area() ? fjJet.area() : 0;
    genJet.setJetArea(jetArea);
    if ( hasBHadron ) {
      genJet.setPdgId(5);
      bjetIdxs.push_back(jets->size());
    }
    else {
      ljetIdxs.push_back(jets->size());
    }

    jets->push_back(genJet);
  }

  // Every building blocks are ready. Continue to pseudo-W and pseudo-top combination
  // NOTE : A C++ trick, use do-while instead of long-nested if-statements.
  do {
    if ( bjetIdxs.size() < 2 ) break;

    // Note : we will do dilepton or semilepton channel only
    if ( leptons->size() == 2 and neutrinos->size() >= 2 ) {
      // Start from dilepton channel
      const int q1 = leptons->at(0).charge();
      const int q2 = leptons->at(1).charge();
      if ( q1*q2 > 0 ) break;

      const auto& lepton1 = q1 > 0 ? leptons->at(0) : leptons->at(1);
      const auto& lepton2 = q1 > 0 ? leptons->at(1) : leptons->at(0);

      double dm = 1e9;
      int selNu1 = -1, selNu2 = -1;
      for ( int i=0, n=neutrinos->size(); i<n; ++i ) {
        const double dm1 = std::abs((lepton1.p4()+neutrinos->at(i).p4()).mass()-wMass_);
        for ( int j=0; j<n; ++j ) {
          if ( i == j ) continue;
          const double dm2 = std::abs((lepton2.p4()+neutrinos->at(j).p4()).mass()-wMass_);
          const double newDm = dm1+dm2;

          if ( newDm < dm ) { dm = newDm; selNu1 = i; selNu2 = j; }
        }
      }
      if ( dm >= 1e9 ) break;

      const auto& nu1 = neutrinos->at(selNu1);
      const auto& nu2 = neutrinos->at(selNu2);
      const auto w1LVec = lepton1.p4()+nu1.p4();
      const auto w2LVec = lepton2.p4()+nu2.p4();

      // Contiue to top quarks
      dm = 1e9; // Reset once again for top combination.
      int selB1 = -1, selB2 = -1;
      for ( auto i : bjetIdxs ) {
        const double dm1 = std::abs((w1LVec+jets->at(i).p4()).mass()-tMass_);
        for ( auto j : bjetIdxs ) {
          if ( i == j ) continue;
          const double dm2 = std::abs((w2LVec+jets->at(j).p4()).mass()-tMass_);
          const double newDm = dm1+dm2;

          if ( newDm < dm ) { dm = newDm; selB1 = i; selB2 = j; }
        }
      }
      if ( dm >= 1e9 ) break;

      const auto& bJet1 = jets->at(selB1);
      const auto& bJet2 = jets->at(selB2);
      const auto t1LVec = w1LVec + bJet1.p4();
      const auto t2LVec = w2LVec + bJet2.p4();

      // Put all of them into candidate collection
      reco::GenParticle t1(q1*2/3., t1LVec, genVertex_, q1*6, 3, false);
      reco::GenParticle w1(q1, w1LVec, genVertex_, q1*24, 3, true);
      reco::GenParticle b1(-q1/3., bJet1.p4(), genVertex_, q1*5, 1, true);
      reco::GenParticle l1(q1, lepton1.p4(), genVertex_, lepton1.pdgId(), 1, true);
      reco::GenParticle n1(0, nu1.p4(), genVertex_, nu1.pdgId(), 1, true);

      reco::GenParticle t2(q2*2/3., t2LVec, genVertex_, q2*6, 3, false);
      reco::GenParticle w2(q2, w2LVec, genVertex_, q2*24, 3, true);
      reco::GenParticle b2(-q1/3., bJet2.p4(), genVertex_, q2*5, 1, true);
      reco::GenParticle l2(q2, lepton2.p4(), genVertex_, lepton2.pdgId(), 1, true);
      reco::GenParticle n2(0, nu2.p4(), genVertex_, nu2.pdgId(), 1, true);

      pseudoTop->push_back(t1);
      pseudoTop->push_back(t2);

      pseudoTop->push_back(w1);
      pseudoTop->push_back(b1);
      pseudoTop->push_back(l1);
      pseudoTop->push_back(n1);

      pseudoTop->push_back(w2);
      pseudoTop->push_back(b2);
      pseudoTop->push_back(l2);
      pseudoTop->push_back(n2);
    }
    else if ( leptons->size() == 1 and neutrinos->size() >= 1 ) {
      // Then continue to the semi-leptonic channel
      const auto& lepton = leptons->at(0);

      double dm = 1e9;
      int selNu = -1, selJ1 = -1, selJ2 = -1;
      for ( int i=0, n=neutrinos->size(); i<n; ++i ) {
        const double dm1 = std::abs((lepton.p4()+neutrinos->at(i).p4()).mass()-wMass_);
        for ( auto j1Itr=ljetIdxs.begin(); j1Itr!=ljetIdxs.end(); ++j1Itr ) {
          const int j1 = *j1Itr;
          const auto& jet1 = jets->at(j1);
          for ( auto j2Itr=std::next(j1Itr); j2Itr!=ljetIdxs.end(); ++j2Itr ) {
            const int j2 = *j2Itr;
            const auto& jet2 = jets->at(j2);
            const double dm2 = std::abs((jet1.p4()+jet2.p4()).mass()-wMass_);
            const double newDm = dm1+dm2;

            if ( newDm < dm ) { dm = newDm; selNu = i; selJ1 = j1; selJ2 = j2; }
          }
        }
      }
      if ( dm >= 1e9 ) break;

      const auto& nu1 = neutrinos->at(selNu);
      const auto& wJet1 = jets->at(selJ1);
      const auto& wJet2 = jets->at(selJ2);
      const auto w1LVec = lepton.p4()+nu1.p4();
      const auto w2LVec = wJet1.p4() + wJet2.p4();

      // Contiue to top quarks
      dm = 1e9; // Reset once again for top combination.
      int selB1 = -1, selB2 = -1;
      for ( auto i : bjetIdxs ) {
        const double dm1 = std::abs((w1LVec+jets->at(i).p4()).mass()-tMass_);
        for ( auto j : bjetIdxs ) {
          if ( i == j ) continue;
          const double dm2 = std::abs((w2LVec+jets->at(j).p4()).mass()-tMass_);
          const double newDm = dm1+dm2;

          if ( newDm < dm ) { dm = newDm; selB1 = i; selB2 = j; }
        }
      }
      if ( dm >= 1e9 ) break;

      const auto& bJet1 = jets->at(selB1);
      const auto& bJet2 = jets->at(selB2);
      const auto t1LVec = w1LVec + bJet1.p4();
      const auto t2LVec = w2LVec + bJet2.p4();

      const int q = lepton.charge();
      // Put all of them into candidate collection
      reco::GenParticle t1(q*2/3., t1LVec, genVertex_, q*6, 3, false);
      reco::GenParticle w1(q, w1LVec, genVertex_, q*24, 3, true);
      reco::GenParticle b1(-q/3., bJet1.p4(), genVertex_, q*5, 1, true);
      reco::GenParticle l1(q, lepton.p4(), genVertex_, lepton.pdgId(), 1, true);
      reco::GenParticle n1(0, nu1.p4(), genVertex_, nu1.pdgId(), 1, true);

      reco::GenParticle t2(-q*2/3., t2LVec, genVertex_, -q*6, 3, false);
      reco::GenParticle w2(-q, w2LVec, genVertex_, -q*24, 3, true);
      reco::GenParticle b2(0, bJet2.p4(), genVertex_, -q*5, 1, true);
      reco::GenParticle u2(0, wJet1.p4(), genVertex_, -2*q, 1, true);
      reco::GenParticle d2(0, wJet2.p4(), genVertex_, q, 1, true);

      pseudoTop->push_back(t1);
      pseudoTop->push_back(t2);

      pseudoTop->push_back(w1);
      pseudoTop->push_back(b1);
      pseudoTop->push_back(l1);
      pseudoTop->push_back(n1);

      pseudoTop->push_back(w2);
      pseudoTop->push_back(b2);
      pseudoTop->push_back(u2);
      pseudoTop->push_back(d2);
    }
  } while (false);

  if ( pseudoTop->size() == 10 ) // If pseudtop decay tree is completed
  {
    // t->W+b
    pseudoTop->at(0).addDaughter(reco::GenParticleRef(pseudoTopRefHandle, 2)); // t->W
    pseudoTop->at(0).addDaughter(reco::GenParticleRef(pseudoTopRefHandle, 3)); // t->b
    pseudoTop->at(2).addMother(reco::GenParticleRef(pseudoTopRefHandle, 0)); // t->W
    pseudoTop->at(3).addMother(reco::GenParticleRef(pseudoTopRefHandle, 0)); // t->b

    // W->lv or W->jj
    pseudoTop->at(2).addDaughter(reco::GenParticleRef(pseudoTopRefHandle, 4));
    pseudoTop->at(2).addDaughter(reco::GenParticleRef(pseudoTopRefHandle, 5));
    pseudoTop->at(4).addMother(reco::GenParticleRef(pseudoTopRefHandle, 2));
    pseudoTop->at(5).addMother(reco::GenParticleRef(pseudoTopRefHandle, 2));

    // tbar->W-b
    pseudoTop->at(1).addDaughter(reco::GenParticleRef(pseudoTopRefHandle, 6));
    pseudoTop->at(1).addDaughter(reco::GenParticleRef(pseudoTopRefHandle, 7));
    pseudoTop->at(6).addMother(reco::GenParticleRef(pseudoTopRefHandle, 1));
    pseudoTop->at(7).addMother(reco::GenParticleRef(pseudoTopRefHandle, 1));

    // W->jj
    pseudoTop->at(6).addDaughter(reco::GenParticleRef(pseudoTopRefHandle, 8));
    pseudoTop->at(6).addDaughter(reco::GenParticleRef(pseudoTopRefHandle, 9));
    pseudoTop->at(8).addMother(reco::GenParticleRef(pseudoTopRefHandle, 6));
    pseudoTop->at(9).addMother(reco::GenParticleRef(pseudoTopRefHandle, 6));
  }

  event.put(neutrinos, "neutrinos");
  event.put(leptons, "leptons");
  event.put(jets, "jets");

  event.put(pseudoTop);
}

const reco::Candidate* PseudoTopProducer::getLast(const reco::Candidate* p)
{
  for ( size_t i=0, n=p->numberOfDaughters(); i<n; ++i )
  {
    const reco::Candidate* dau = p->daughter(i);
    if ( p->pdgId() == dau->pdgId() ) return getLast(dau);
  }
  return p;
}

bool PseudoTopProducer::isFromHadron(const reco::Candidate* p) const
{
  for ( size_t i=0, n=p->numberOfMothers(); i<n; ++i )
  {
    const reco::Candidate* mother = p->mother(i);
    if ( mother->numberOfMothers() == 0 ) continue; // Skip incident beam
    const int pdgId = abs(mother->pdgId());

    if ( pdgId > 100 ) return true;
    else if ( isFromHadron(mother) ) return true;
  }
  return false;
}

bool PseudoTopProducer::isBHadron(const reco::Candidate* p) const
{
  const unsigned int absPdgId = abs(p->pdgId());
  if ( !isBHadron(absPdgId) ) return false;

  // Do not consider this particle if it has B hadron daughter
  // For example, B* -> B0 + photon; then we drop B* and take B0 only
  for ( int i=0, n=p->numberOfDaughters(); i<n; ++i )
  {
    const reco::Candidate* dau = p->daughter(i);
    if ( isBHadron(abs(dau->pdgId())) ) return false;
  }

  return true;
}

bool PseudoTopProducer::isBHadron(const unsigned int absPdgId) const
{
  if ( absPdgId <= 100 ) return false; // Fundamental particles and MC internals
  if ( absPdgId >= 1000000000 ) return false; // Nuclei, +-10LZZZAAAI

  // General form of PDG ID is 7 digit form
  // +- n nr nL nq1 nq2 nq3 nJ
  //const int nJ = absPdgId % 10; // Spin
  const int nq3 = (absPdgId / 10) % 10;
  const int nq2 = (absPdgId / 100) % 10;
  const int nq1 = (absPdgId / 1000) % 10;

  if ( nq3 == 0 ) return false; // Diquarks
  if ( nq1 == 0 and nq2 == 5 ) return true; // B mesons
  if ( nq1 == 5 ) return true; // B baryons

  return false;
}

reco::GenParticleRef PseudoTopProducer::buildGenParticle(const reco::Candidate* p, reco::GenParticleRefProd& refHandle,
                                                               std::auto_ptr<reco::GenParticleCollection>& outColl) const
{
  reco::GenParticle pOut(*dynamic_cast<const reco::GenParticle*>(p));
  pOut.clearMothers();
  pOut.clearDaughters();
  pOut.resetMothers(refHandle.id());
  pOut.resetDaughters(refHandle.id());

  outColl->push_back(pOut);

  return reco::GenParticleRef(refHandle, outColl->size()-1);
}

