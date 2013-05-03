#include <boost/thread/thread.hpp>
#include <boost/lockfree/queue.hpp>

#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <sstream>
#include <memory>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <chrono>
#include <iomanip>

#include <boost/program_options.hpp>
#include <boost/program_options/parsers.hpp>

#include "g2logworker.h"
#include "g2log.h"

#include <jellyfish/sequence_parser.hpp>
#include <jellyfish/parse_read.hpp>
#include <jellyfish/mer_counting.hpp>
#include <jellyfish/misc.hpp>
#include <jellyfish/compacted_hash.hpp>

#include "BiasIndex.hpp"
#include "utils.hpp"
#include "genomic_feature.hpp"
#include "CountDBNew.hpp"
#include "collapsed_iterative_optimizer.hpp"
//#include "iterative_optimizer.hpp"
#include "tclap/CmdLine.h"

int runIterativeOptimizer(int argc, char* argv[] ) {
  using std::string;
  namespace po = boost::program_options;

  try{

   bool poisson = false;

   uint32_t maxThreads = std::thread::hardware_concurrency();

    po::options_description generic("Command Line Options");
    generic.add_options()
      ("version,v", "print version string")
      ("help,h", "produce help message")
      ("cfg,f", po::value< string >(), "config file")
    ;

    po::options_description config("Configuration");
    config.add_options()
      ("genes,g", po::value< std::vector<string> >(), "gene sequences")
      ("counts,c", po::value<string>(), "count file")
      ("index,i", po::value<string>(), "sailfish index prefix (without .sfi/.sfc)")
      ("bias,b", po::value<string>(), "bias index prefix (without .bin/.dict)")
      //("thash,t", po::value<string>(), "transcript jellyfish hash file")
      ("output,o", po::value<string>(), "output file")
      ("tgmap,m", po::value<string>(), "file that maps transcripts to genes")
      ("filter,f", po::value<double>()->default_value(0.0), "during iterative optimization, remove transcripts with a mean less than filter")
      ("iterations,i", po::value<size_t>(), "number of iterations to run the optimzation")
      ("lutfile,l", po::value<string>(), "Lookup table prefix")
      ("threads,p", po::value<uint32_t>()->default_value(maxThreads), "The number of threads to use when counting kmers")
      ;

    po::options_description programOptions("combined");
    programOptions.add(generic).add(config);

    po::variables_map vm;
    po::store(po::command_line_parser(argc, argv).options(programOptions).run(), vm);
 
    //bool poisson = ( vm.count("poisson") ) ? true : false;
    

    if ( vm.count("help") ){
      std::cout << "Sailfish\n";
      std::cout << programOptions << std::endl;
      std::exit(1);
    }

    if ( vm.count("cfg") ) {
      std::cerr << "have detected configuration file\n";
      string cfgFile = vm["cfg"].as<string>();
      std::cerr << "cfgFile : [" << cfgFile << "]\n";
      po::store(po::parse_config_file<char>(cfgFile.c_str(), programOptions, true), vm);
    }
    po::notify(vm);

    string transcriptGeneMap = vm["tgmap"].as<string>();
    string hashFile = vm["counts"].as<string>();
    std::vector<string> genesFile = vm["genes"].as<std::vector<string>>();
    //string transcriptHashFile = vm["thash"].as<string>();
    string sfIndexBase = vm["index"].as<string>();
    string sfIndexFile = sfIndexBase+".sfi";
    string sfTrascriptCountFile = sfIndexBase+".sfc";
    string outputFile = vm["output"].as<string>();
    double minMean = vm["filter"].as<double>();
    string lutprefix = vm["lutfile"].as<string>();
    auto tlutfname = lutprefix + ".tlut";
    auto klutfname = lutprefix + ".klut";

    typedef GenomicFeature<TranscriptGeneID> CustomGenomicFeature;
    /*
    std::ifstream transcriptGeneFile(transcriptGeneMap );
    std::vector< CustomGenomicFeature > features;
    CustomGenomicFeature feat;
    std::cerr << "parsing gtf file [" << transcriptGeneMap  << "] . . . ";
    while ( transcriptGeneFile >> feat ) { 
      features.push_back(feat);
    };
    std::cerr << "done\n";
    transcriptGeneFile.close();
    */
    std::cerr << "parsing gtf file [" << transcriptGeneMap  << "] . . . ";
    auto features = GTFParser::readGTFFile<TranscriptGeneID>(transcriptGeneMap);
    std::cerr << "done\n";

    std::cerr << "building transcript to gene map . . .";
    auto tgm = utils::transcriptToGeneMapFromFeatures( features );
    std::cerr << "done\n";

    std::cerr << "Reading transcript index from [" << sfIndexFile << "] . . .";
    auto sfIndex = PerfectHashIndex::fromFile( sfIndexFile );
    auto del = []( PerfectHashIndex* h ) -> void { /*do nothing*/; };
    auto sfIndexPtr = std::shared_ptr<PerfectHashIndex>( &sfIndex, del );
    std::cerr << "done\n";

    /*
    std::cerr << "Reading transcript counts from [" << sfTrascriptCountFile << "] . . .";
    auto transcriptHash = CountDBNew::fromFile(sfTrascriptCountFile, sfIndexPtr);
    std::cerr << "done\n";
    */
   
    // the READ hash
    std::cerr << "Reading read counts from [" << hashFile << "] . . .";
    auto hash = CountDBNew::fromFile( hashFile, sfIndexPtr );
    std::cerr << "done\n";
    const std::vector<string>& geneFiles{genesFile};
    auto merLen = sfIndex.kmerLength();
    
    BiasIndex bidx = vm.count("bias") ? BiasIndex( vm["bias"].as<string>() ) : BiasIndex();

    std::cerr << "Creating optimizer . . .";
    uint32_t numThreads = vm["threads"].as<uint32_t>();
    CollapsedIterativeOptimizer<CountDBNew> solver(hash, tgm, bidx, numThreads);
    // IterativeOptimizer<CountDBNew, CountDBNew> solver( hash, transcriptHash, tgm, bidx );
    std::cerr << "done\n";

    if ( poisson ) {
      std::cerr << "optimizing using Poisson model\n";
      // for IterativeOptimizer
      // solver.optimizePoisson( geneFiles, outputFile );
    } else {
      size_t numIter = vm["iterations"].as<size_t>();
      std::cerr << "optimizing using iterative optimization [" << numIter << "] iterations";
      // for CollapsedIterativeOptimizer (EM algorithm)
      solver.optimize(klutfname, tlutfname, outputFile, numIter, minMean );
      // for LASSO Iterative Optimizer
      //solver.optimizeNNLASSO(klutfname, tlutfname, outputFile, numIter, minMean );
      // for IterativeOptimizer
      // solver.optimize( geneFiles, outputFile, numIter, minMean );
    }

    

  } catch (po::error &e){
    std::cerr << "exception : [" << e.what() << "]. Exiting.\n";
    std::exit(1);
  }

}

int help(int argc, char* argv[]) {
  auto helpmsg = R"(
  Sailfish v0.1
  =============

  Please invoke sailfish with one of the following quantification
  methods {itopt, setcover, nnls}.  For more inforation on the 
  options for theses particular methods, use the -h flag along with
  the method name.  For example:

  Sailfish itopt -h

  will give you detailed help information about the iterative optimization
  method.
  )";

  std::cerr << helpmsg << "\n";
  return 1;
}

//int indexMain( int argc, char* argv[] );
int mainIndex( int argc, char* argv[] );
int mainCount( int argc, char* argv[] );

int main( int argc, char* argv[] ) {

  g2LogWorker logger(argv[0], "./" );
  g2::initializeLogging(&logger);
  std::cerr << "** log file being written to " << logger.logFileName().get() << "** \n";
  
  using std::string;
  namespace po = boost::program_options;

  try {
    std::unordered_map<std::string, std::function<int(int, char*[])>> cmds({
      {"-h" , help},
      {"--help", help},
      {"itopt", runIterativeOptimizer},
      {"index", mainIndex},
      {"count", mainCount}});
    
    char** argv2 = new char*[argc-1];
    argv2[0] = argv[0];
    std::copy_n( &argv[2], argc-2, &argv2[1] );
    if (cmds.find(argv[1]) == cmds.end()) {
      help( argc-1, argv2 );
    } else {
      cmds[ argv[1] ](argc-1, argv2);
    }
    delete[] argv2;

  } catch (TCLAP::ArgException &e) {
    std::cerr << "error: " << e.error() << " for arg " << e.argId() << std::endl;
  }


return 0;
}