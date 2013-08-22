// xsplit.cc: implementation of class form_finder
//////////////////////////////////////////////////////////////////////////
//
// Copyright 1990-2012 John Cremona
// 
// This file is part of the eclib package.
// 
// eclib is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the
// Free Software Foundation; either version 2 of the License, or (at your
// option) any later version.
// 
// eclib is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
// for more details.
// 
// You should have received a copy of the GNU General Public License
// along with eclib; if not, write to the Free Software Foundation,
// Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
// 
//////////////////////////////////////////////////////////////////////////
 
#include <unistd.h>  // for unlink() (not needed on linux)

#define USE_SPARSE 1
#define ECLIB_INT_NUM_THREADS 15
#include <eclib/logger.h>
#include <eclib/xsplit.h>

#include <eclib/smatrix_elim.h>
subspace sparse_combine(const subspace& s1, const subspace& s2);
mat sparse_restrict(const mat& m, const subspace& s);
smat restrict_mat(const smat& m, const subspace& s);

// CLASS FORM_FINDER (was called splitter)

form_finder::form_finder(splitter_base* hh, int plus, int maxd, int mind, int dualflag, int bigmatsflag, int v)
:h(hh), plusflag(plus), dual(dualflag), bigmats(bigmatsflag), verbose(v), 
 maxdepth(maxd), mindepth(mind), gnfcount(0)
{
  eclogger::setLevel( verbose );

  denom1 = h->matden();
  dimen  = h->matdim();
 
  // Create and initialise new data object as root node
  // passing a constant pointer of current form_finder object
  // to data class constructor
  root = new ff_data( this );

  // Set initial values
  // form_finder class is a friend of ff_data class
  // so may access private members
  root -> subdim_ = dimen;
 
  targetdim = 1;
  if( !plusflag ) {           // full conjmat not needed when plusflag is true
    targetdim=2;
    if( bigmats ) root -> conjmat_ = h -> s_opmat(-1,dual); 
  }
}

form_finder::~form_finder(void) {
  // Decendants of root node will be recursively deleted
  // if they have not already been deleted during find()
  // All dynamically created objects (subspaces) held 
  // in each data node will also be deleted.
  delete root;
}

void form_finder::make_opmat(long i, ff_data &data) { 
  data.the_opmat_ = h -> s_opmat(i,dual,verbose); 
}

void form_finder::make_submat( ff_data &data ) {
  // Cache current data node depth
  long depth = data.depth_;

  if( bigmats ) { 
    // fetch the_opmat from file, or compute
    make_opmat(depth,data);
    
    if( depth == 0 ) data.submat_ = data.the_opmat_;
    else {
	    ECLOG(1) << "restricting the_opmat to subspace...";
	    data.submat_ = restrict_mat(data.the_opmat_,*(data.nest_));
	    ECLOG(1) << "done." << endl;
	  }
      
    data.the_opmat_ = smat(0,0); // releases its space
  }
  else {
    if( nrows(data.submat_) == 0 ) {
	    if( depth == 0 ) data.submat_ = h -> s_opmat(depth,1,verbose);
      else             data.submat_ = h -> s_opmat_restricted(depth,*(data.nest_),1,verbose);
	  }
  }
}

/**
 * go_down()
 *
 * Initiates creation of new subspace; data stored in new
 * data node. Data node passed as parameter will become the
 * _parent_ of this new data node.
 */
void form_finder::go_down(ff_data &data, long eig, int last) {
  // Cache current depth
  long depth = data.depth_;

  // Locate required child w.r.t test eigenvalue
  ff_data *child = data.child( eig ); 

  // Set new depth
  child -> depth_ = depth + 1;
   
  SCALAR eig2 = eig*denom1;

  ECLOG(1) << "Increasing depth to " << depth+1 << ", "
           << "trying eig = " << eig << "..."
           << "after scaling, eig =  " << eig2 << "..." << endl;
  // if(depth) eig2*= denom(*nest[depth]); // else latter is 1 anyway
  //                        ^ data.nest_
  ssubspace s(0);

  vector<int> submat_dim = dim(data.submat_);
  stringstream submat_dim_ss;
  std::copy(submat_dim.begin(),submat_dim.end(),ostream_iterator<int>(submat_dim_ss," "));

  ECLOG(1) << "Using sparse elimination (size = [ "
                     << submat_dim_ss.str() << "], density ="
		                 << density(data.submat_) << ")..." << endl;
  ECLOG(3) << "submat = " << data.submat_ << flush;

  s = eigenspace(data.submat_,eig2);

  // Increment data usage counter for parent node
  data.increaseSubmatUsage();

  // Reset current submat if all children have used it
  // Save space (will recompute when needed)
  //if(    ( depth == 0 )
  //    && ( dim(s) > 0 )
  //    && ( nrows(data.submat_) > 1000 )
  //    && ( data.submatUsage_ == data.numChildren_ ) ) {
  //  data.submat_ = smat(0,0); 
  //}

  ECLOG(1) << "done (dim = " << dim(s) << "), combining subspaces..." << flush;
  
  if( depth == 0 ) child -> nest_ = new ssubspace(s);
  else             child -> nest_ = new ssubspace(combine( *(data.nest_),s ));
  
  ECLOG(1) << "done." << endl;
  
  depth++; // Local depth increment (does not effect data nodes)

  child -> subdim_ = dim( *(child -> nest_) );

  ECLOG(1) << "Eigenvalue " << eig 
           << " has multiplicity " << child -> subdim_ << endl;
  if(child -> subdim_>0) {
    ECLOG(0) << " eig " << eig 
             << " gives new subspace at depth " << depth
             << " of dimension " << child -> subdim_ << endl;
  }
}

void form_finder::go_up( ff_data &data ) {
  // Cache pointer to parent data node for access after current node is deleted
  ff_data *parent = data.parent_;

#ifdef MULTITHREAD
  // Lock parent node with scoped lock 
  boost::mutex::scoped_lock lock( parent -> go_up_lock_ );
#ifdef ECLIB_MULTITHREAD_DEBUG
  ECLOG(1) <<"in go_up for eig="<<data.eigenvalue_<<" depth="<<data.depth_<<" status="<<data.status_<<std::endl;
#endif
#endif

  long eig=data.eigenvalue_;

  // Erasing node via children array of parent which calls destructor
  // of object (ff_data), using eigenvalue as key
  parent -> childStatus( data.eigenvalue_, COMPLETE );
  parent -> eraseChild( data.eigenvalue_ );

#ifdef MULTITHREAD
  // Only last child to complete will execute the following
  // Detects if parent is root node
  if( parent -> complete() && parent -> parent_ != NULL ) {
    lock.unlock();
    go_up( *parent );
  }
#endif
}

void form_finder::make_basis( ff_data &data ) {
  // Cache data values
  long depth  = data.depth();
  long subdim = data.subdim();
  vector< long > eiglist = data.eiglist();

  if( subdim != targetdim ) {
    cout << "error in form_finder::make_basis with eiglist = ";
    for(int i=0; i<depth; i++) 
      cout << eiglist[i] << ",";
    cout << "\nfinal subspace has dimension " << subdim << endl;
    cout << "aborting this branch!" << endl;
    return;
  }

  if(plusflag) {
    // must treat separately since we did not
    // define nest[0] in order to save space
    if(depth==0) {
      data.bplus_    = vec(dimen); 
      data.bplus_[1] = 1;
	  }
    else {
      data.bplus_ = getbasis1(data.nest_);
    }
     
    return;
  }

  ssubspace* s = data.nest_;  // only used when depth>0
  ssubspace *spm0, *spm;
  SCALAR eig = denom1;
  // if(depth) eig*=denom(*s);
  smat subconjmat;            // only used when depth>0
  if( bigmats ) {
    subconjmat = (depth) ? restrict_mat(data.conjmat_, *s) : data.conjmat_;
    // will only be a 2x2 in this case (genus 1 only!)
  }
  else {
    subconjmat = h->s_opmat_restricted(-1,*s,1,verbose);
  }

  // C++11 loop over two variables (similar to python)
  // for( int b : { -1,+1 } ) { /* use b as -1 or +1 */ }
  for(long signeig=+1; signeig>-2; signeig-=2) {
    SCALAR seig; 
           seig = eig;
    
    if(signeig<0) seig =- eig;
    
    if(depth) {
	    spm0 = new ssubspace(eigenspace(subconjmat,seig));
	    spm  = new ssubspace(combine(*s,*spm0));
	    delete spm0;
    }
    else {
      spm = new ssubspace(eigenspace(subconjmat,seig));
    }
    
    if(dim(*spm)!=1) {
      cout << "error in form_finder::makebasis; ";
      cout << "\nfinal (";
      
      if(signeig>0) cout << "+"; 
      else cout << "-";
        
      cout << ") subspace has dimension " << dim(*spm) << endl;
      cout << "aborting this branch!" << endl;
	    delete spm;
      return;
    }
    
    if(signeig>0) data.bplus_  = getbasis1(spm);
    else          data.bminus_ = getbasis1(spm);

    delete spm;
  }
}

vec form_finder::getbasis1(const ssubspace* s)
{
  VEC b = basis(*s).as_mat().col(1);
#ifdef MODULAR
  if(!liftok(b,MODULUS)) 
     cout << "Unable to lift eigenvector from mod " << MODULUS << endl;
#else
  makeprimitive(b);
#endif
#ifdef MULTI
  scalar n=0; // dummy variable to gt the right type in next line
  return b.shorten(n);
#else      
  return b;
#endif
}

void form_finder::recover(vector< vector<long> > eigs) {
  for(unsigned int iform=0; iform<eigs.size(); iform++) {
    if(verbose) {
	    cout << "Form number " << iform+1 << " with eigs ";
	    
      int n = eigs[iform].size(); 
      if(n>10) n = 10;

	    copy(eigs[iform].begin(), eigs[iform].begin() + n, 
	       ostream_iterator<long>(cout, " ")); 
	    cout << "..." << endl;
	  }
 
    splitoff(eigs[iform]);
  }  
}

void form_finder::splitoff(const vector<long>& eigs) {

  // Always start at root node
  current = root;

  // Temporary variables
  long depth  = current -> depth_;
  long subdim = current -> subdim_; 

  if( verbose ) {
    cout << "Entering form_finder, depth = " << depth 
         << ", dimension " << subdim << endl;
  }

  // Walk down nodes (if any already created) for common branches
  while( current -> children_[eigs[depth]] != NULL ) {
    // Update current node pointer
    current = current -> children_[eigs[depth]];

    // Update new depth (should always be depth+1)
    depth = current -> depth_;

    // Update new subdimension
    subdim = current -> subdim_;
  }

  // Current node is new branch point
  // We want to trim old branches to save memory ...
  // (calling clear also calls destructor on objects)
  current -> children_.clear(); 

  if( verbose ) {
    cout << "restarting at depth = " << depth << ", "
         << "dimension " << subdim << endl;
  }
  
  // ... and grow a new branch down to required depth.
  while( (subdim > targetdim) && (depth < maxdepth) ) {
    // Create new child node
    ff_data *child = new ff_data( this );

    // Configure data node ancestry
    current -> addChild( eigs[depth], *child );

    // Proceed to go down
    go_down(*current,eigs[depth],1);

    // Cache new values
    depth  = current -> depth_;
    subdim = current -> subdim_;
  }

  // Creating newforms
  // No need to call store() first since
  // we call this function in serial
  make_basis(*current);
  h->use(current -> bplus_,current -> bminus_,eigs); 
  
  return;
}

void form_finder::find() {
#ifdef MULTITHREAD
  // Set number of threads to use either through default
  // ECLIB_INT_NUM_THREADS macro defined above, or
  // ECLIB_EXT_NUM_THREADS environment variable.
  unsigned int eclib_num_threads = ECLIB_INT_NUM_THREADS;

  stringstream s;
  s << getenv("ECLIB_EXT_NUM_THREADS");
  if( !s.str().empty() ) eclib_num_threads = atoi(s.str().c_str());

  // Start job queue. We keep job queue local to ensure threads are 
  // not kept busy for longer than necessary.
  pool.start( eclib_num_threads, verbose );
#endif

  // Proceed in recursive find, passing a node through
  //find( *root );
  current = root;
  find( *current );
  
#ifdef MULTITHREAD
  // Join all threads in threadpool to wait for all jobs to finish
  // Or detect when all branches of the tree has been traversed
  pool.close();
#endif

  // Now compute all newforms only if recursion has finished
  if(verbose>1) cout << "Now performing use() on all lists at once" << endl;
  for( int nf = 0; nf < gnfcount; nf++ ) {
    h-> use(gbplus[nf],gbminus[nf],gaplist[nf]);
  }
}

void form_finder::find( ff_data &data ) {
  // Cache values of current data
  long depth  = data.depth();
  long subdim = data.subdim();
  vector< long > eiglist = data.eiglist();

  vector<long> subeiglist(eiglist.begin(),eiglist.begin()+depth);

 
  int dimold = h->dimoldpart(subeiglist);

  stringstream subeiglist_ss;
  std::copy(subeiglist.begin(),subeiglist.end(),ostream_iterator<long>(subeiglist_ss," "));

  ECLOG(0) << "In formfinder, depth = " << depth 
           << ", aplist = [ " << subeiglist_ss.str() << "];\t"
           << "dimsofar=" << subdim
           << ", dimold=" << dimold
           << ", dimnew=" << subdim-dimold << "\n";
 
  if( dimold == subdim ) {
    data.setStatus( ALL_OLD );     // Set status of current node
    ECLOG(0) << "Abandoning a common eigenspace of dimension " << subdim
             << " which is a sum of oldclasses." << endl;
    return;   // This branch of the recursion ends: all is old
  }

  if( ( subdim == targetdim ) && ( depth > mindepth ) ) { 
    data.setStatus( FOUND_NEW );   // Set status of current node
    make_basis( data );
    store(data.bplus_,data.bminus_,subeiglist);
    return;
  }

  if( depth == maxdepth ) { 
    data.setStatus( MAX_DEPTH );
    if(1) {       // we want to see THIS message whatever the verbosity level! 
      cout << "\nFound a " << subdim << "D common eigenspace\n";
      cout << "Abandoning, even though oldforms only make up ";
      cout << dimold << "D of this." << endl;
    }
    return;
  }

  // Pass data node through to make_submat()
  // NOTE originally called in go_down(), but relocated here since
  // it only needs to be called once per node. 
  make_submat(data);

  // The recursive part:
  vector<long> t_eigs = h->eigrange(depth);
  vector<long>::const_iterator apvar = t_eigs.begin();

  stringstream t_eigs_ss;
  std::copy(t_eigs.begin(),t_eigs.end(),ostream_iterator<long>(t_eigs_ss," "));

  ECLOG(0) << "Testing eigenvalues [ " << t_eigs_ss.str() 
           << "] at level " << (depth+1) << endl;

  // Set children counter
  data.numChildren( t_eigs.size() );

  while( apvar != t_eigs.end() ) { 
    ECLOG(1) << "Going down with ap = " << (*apvar) <<endl;
    long eig = *apvar++;

    // Initiate new data node, passing a constant reference of the current 
    // form_finder object to the data class constructor
    ff_data *child = new ff_data( this );

    // Configure data node ancestry
    data.addChild( eig, *child );

#ifdef MULTITHREAD
    // Post newly created child node to threadpool
    pool.post< ff_data >( *child );

    // Include following when controlling depth of parallelisation
    // work in serial instead ... no point adding to job queue
    //go_down(data,eig,apvar==t_eigs.end());
    //if( child -> subdim_ > 0 ) find( *child );
    //if( data.status_ != INTERNAL ) go_up( *child );
#else   
    // Pass through current data node and new test eigenvalue to go_down()
    go_down( data, eig, apvar==t_eigs.end() );
    
    // We pass new child node to find() 
    if( child -> subdim_ > 0 ) find( *child );

    go_up( *child );
#endif
  }  

#ifndef MULTITHREAD
  ECLOG(0) << "Finished at level " << (depth+1) << endl;
#endif
}

void form_finder::store(vec bp, vec bm, vector<long> eigs) {
#ifdef MULTITHREAD
  // Lock function
  boost::mutex::scoped_lock lock( store_lock );
#endif

  // Store sub-bplus,bminus,eiglists in class level containers
  gbplus.push_back(bp);
  gbminus.push_back(bm);
  gaplist.push_back(eigs);

  // Increment global counter
  gnfcount++;

  // Inform about newform count
  ECLOG(0) << "Current newform subtotal count at " << gnfcount << endl;
}

#if (METHOD==2)
subspace sparse_combine(const subspace& s1, const subspace& s2)
{
  // we assume s1, s2 are subspace mod BIGPRIME!
   scalar d = denom(s1)*denom(s2);
   const smat& sm1(basis(s1)), sm2(basis(s2));
   const mat&  b = (sm1*sm2).as_mat(); 
   const vec&  p = pivots(s1)[pivots(s2)];
   return subspace(b,p,d);
   //  return COMBINE(s1,s2);
}

mat sparse_restrict(const mat& m, const subspace& s)
{
  if(dim(s)==nrows(m)) return m; // trivial special case, s is whole space
  scalar dd = denom(s);  // will be 1 if s is a mod-p subspace
  mat b(basis(s));
  smat sm(m), sb(b);
  vec piv=pivots(s);
  smat smr = sm.select_rows(piv);
  smat ans = smr*sb;
  int check=0;
  if(check) {
    smat left = sm*sb; 
    if(dd!=1) {cout<<"(dd="<<dd<<")"; left.mult_by_scalar_mod_p(dd);}
    smat right = sb*ans;
    int ok = eqmodp(left,right);
    if (!ok) 
    {
      cout<<"Warning from sparse_restrict: subspace not invariant!\n";
      cout<<"Difference = \n"<<left-right<<endl;
      abort();
    }
  }
  check=0;
  if(check) {
    int ok = (ans.as_mat()==RESTRICT(m,s));
    if(!ok)
    {
      cout<<"Error in sparse_restrict: sparse result differs fromnormal!\n";
      abort();
    }
  }
  return ans.as_mat();
}

smat restrict_mat(const smat& m, const subspace& s)
{
  if(dim(s)==nrows(m)) return m; // trivial special case, s is whole space
  return mult_mod_p(m.select_rows(pivots(s)),smat(basis(s)),BIGPRIME);
}

#endif

// end of XSPLIT.CC
