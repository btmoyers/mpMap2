#include "imputeFounders.h"
#include "intercrossingAndSelfingGenerations.h"
#include "recodeFoundersFinalsHets.h"
#include "matrices.hpp"
#include "probabilities.hpp"
#include "probabilities2.h"
#include "probabilities4.h"
#include "probabilities8.h"
#include "probabilities16.h"
#include "funnelsToUniqueValues.h"
#include "estimateRFCheckFunnels.h"
#include "markerPatternsToUniqueValues.h"
#include "intercrossingHaplotypeToMarker.hpp"
#include "funnelHaplotypeToMarker.hpp"
#include "viterbi.hpp"
#include "recodeHetsAsNA.h"
template<int nFounders, bool infiniteSelfing> void imputedFoundersInternal2(Rcpp::IntegerMatrix founders, Rcpp::IntegerMatrix finals, Rcpp::S4 pedigree, Rcpp::List hetData, Rcpp::List map, Rcpp::IntegerMatrix results, double homozygoteMissingProb, double heterozygoteMissingProb, Rcpp::IntegerMatrix key)
{
	//Work out maximum number of markers per chromosome
	int maxChromosomeMarkers = 0;
	for(int i = 0; i < map.size(); i++)
	{
		Rcpp::NumericVector chromosome = map(i);
		maxChromosomeMarkers = std::max((int)chromosome.size(), maxChromosomeMarkers);
	}

	typedef typename expandedProbabilities<nFounders, infiniteSelfing>::type expandedProbabilitiesType;
	//expandedProbabilitiesType haplotypeProbabilities;

	Rcpp::Function diff("diff"), haldaneToRf("haldaneToRf");

	//Get out generations of selfing and intercrossing
	std::vector<int> intercrossingGenerations, selfingGenerations;
	getIntercrossingAndSelfingGenerations(pedigree, finals, nFounders, intercrossingGenerations, selfingGenerations);

	int maxSelfing = *std::max_element(selfingGenerations.begin(), selfingGenerations.end());
	int minSelfing = *std::min_element(selfingGenerations.begin(), selfingGenerations.end());
	int maxAIGenerations = *std::max_element(intercrossingGenerations.begin(), intercrossingGenerations.end());
	int minAIGenerations = *std::min_element(intercrossingGenerations.begin(), intercrossingGenerations.end());
	minAIGenerations = std::max(minAIGenerations, 1);
	int nMarkers = founders.ncol();
	int nFinals = finals.nrow();

	//re-code the founder and final marker genotypes so that they always start at 0 and go up to n-1 where n is the number of distinct marker alleles
	//We do this to make it easier to identify markers with identical segregation patterns. recodedFounders = column major matrix
	Rcpp::IntegerMatrix recodedFounders(nFounders, nMarkers), recodedFinals(nFinals, nMarkers);
	Rcpp::List recodedHetData(nMarkers);
	recodedHetData.attr("names") = hetData.attr("names");
	recodedFinals.attr("dimnames") = finals.attr("dimnames");

	recodeDataStruct recoded;
	recoded.recodedFounders = recodedFounders;
	recoded.recodedFinals = recodedFinals;
	recoded.founders = founders;
	recoded.finals = finals;
	recoded.hetData = hetData;
	recoded.recodedHetData = recodedHetData;
	recodeFoundersFinalsHets(recoded);

	if(infiniteSelfing)
	{
		bool foundHets = replaceHetsWithNA(recodedFounders, recodedFinals, recodedHetData);
		if(foundHets)
		{
			Rcpp::Function warning("warning");
			//Technically a warning could lead to an error if options(warn=2). This would be bad because it would break out of our code. This solution generates a c++ exception in that case, which we can then ignore. 
			try
			{
				warning("Input data had heterozygotes but was analysed assuming infinite selfing. All heterozygotes were ignored. \n");
			}
			catch(...)
			{}
		}
		std::fill(selfingGenerations.begin(), selfingGenerations.end(), 0);
	}

	//Get out the number of unique funnels. This is only needed because in the case of one funnel we assume a single funnel design and in the case of multiple funnels we assume a random funnels design
	std::vector<std::string> errors, warnings;
	std::vector<funnelType> allFunnels, lineFunnels;
	{
		estimateRFCheckFunnels(recodedFinals, recodedFounders, recodedHetData, pedigree, intercrossingGenerations, warnings, errors, allFunnels, lineFunnels);
		if(errors.size() > 0)
		{
			std::stringstream ss;
			for(std::size_t i = 0; i < errors.size(); i++)
			{
				ss << errors[i] << std::endl;
			}
			throw std::runtime_error(ss.str().c_str());
		}
		//Don't bother outputting warnings here
	}
	//map containing encodings of the funnels involved in the experiment (as key), and an associated unique index (again, using the encoded values directly is no good because they'll be all over the place). Unique indices are contiguous again.
	std::map<funnelEncoding, funnelID> funnelTranslation;
	//vector giving the funnel ID for each individual
	std::vector<funnelID> lineFunnelIDs;
	//vector giving the encoded value for each individual
	std::vector<funnelEncoding> lineFunnelEncodings;
	//vector giving the encoded value for each value in allFunnels
	std::vector<funnelEncoding> allFunnelEncodings;
	funnelsToUniqueValues(funnelTranslation, lineFunnelIDs, lineFunnelEncodings, allFunnelEncodings, lineFunnels, allFunnels, nFounders);
	
	unsigned int maxAlleles = recoded.maxAlleles;
	if(maxAlleles > 64)
	{
		throw std::runtime_error("Internal error - Cannot have more than 64 alleles per marker");
	}

	markerPatternsToUniqueValuesArgs markerPatternData;
	markerPatternData.nFounders = nFounders;
	markerPatternData.nMarkers = nMarkers;
	markerPatternData.recodedFounders = recodedFounders;
	markerPatternData.recodedHetData = recodedHetData;
	markerPatternsToUniqueValues(markerPatternData);

	//Intermediate results. These give the most likely paths from the start of the chromosome to a marker, assuming some value for the underlying founder at the marker
	Rcpp::IntegerMatrix intermediate(nFounders, maxChromosomeMarkers);
	int cumulativeMarkerCounter = 0;

	xMajorMatrix<expandedProbabilitiesType> intercrossingHaplotypeProbabilities(maxChromosomeMarkers-1, maxAIGenerations - minAIGenerations + 1, maxSelfing - minSelfing+1);
	rowMajorMatrix<expandedProbabilitiesType> funnelHaplotypeProbabilities(maxChromosomeMarkers-1, maxSelfing - minSelfing + 1);

	//The single loci probabilities are different depending on whether there are zero or one generations of intercrossing. But once you have non-zero generations, it doesn't matter how many
	std::vector<array2<nFounders> > intercrossingSingleLociHaplotypeProbabilities(maxSelfing - minSelfing+1);
	std::vector<array2<nFounders> > funnelSingleLociHaplotypeProbabilities(maxSelfing - minSelfing + 1);

	int nFunnels = (int)allFunnelEncodings.size();
	//Generate single loci genetic data
	for(int selfingGenerationCounter = minSelfing; selfingGenerationCounter <= maxSelfing; selfingGenerationCounter++)
	{
		array2<nFounders>& funnelArray = funnelSingleLociHaplotypeProbabilities[selfingGenerationCounter - minSelfing];
		array2<nFounders>& intercrossingArray = intercrossingSingleLociHaplotypeProbabilities[selfingGenerationCounter - minSelfing];
		singleLocusGenotypeProbabilitiesNoIntercross<nFounders, infiniteSelfing>(funnelArray, selfingGenerationCounter, nFunnels);
		singleLocusGenotypeProbabilitiesWithIntercross<nFounders, infiniteSelfing>(intercrossingArray, selfingGenerationCounter, nFunnels);
		//Take logarithms
		for(int i = 0; i < nFounders; i++)
		{
			for(int j = 0; j < nFounders; j++)
			{
				if(funnelArray.values[i][j] == 0) funnelArray.values[i][j] = -std::numeric_limits<double>::infinity();
				else funnelArray.values[i][j] = log(funnelArray.values[i][j]);
				if(intercrossingArray.values[i][j] == 0) intercrossingArray.values[i][j] = -std::numeric_limits<double>::infinity();
				else intercrossingArray.values[i][j] = log(intercrossingArray.values[i][j]);
			}
		}
	}

	//We'll do a dispath based on whether or not we have infinite generations of selfing. Which requires partial template specialization, which requires a struct/class
	viterbiAlgorithm<nFounders, infiniteSelfing> viterbi(markerPatternData, intercrossingHaplotypeProbabilities, funnelHaplotypeProbabilities, maxChromosomeMarkers);
	viterbi.recodedHetData = recodedHetData;
	viterbi.recodedFounders = recodedFounders;
	viterbi.recodedFinals = recodedFinals;
	viterbi.lineFunnelIDs = &lineFunnelIDs;
	viterbi.lineFunnelEncodings = &lineFunnelEncodings;
	viterbi.intercrossingGenerations = &intercrossingGenerations;
	viterbi.selfingGenerations = &selfingGenerations;
	viterbi.results = results;
	viterbi.key = key;
	viterbi.homozygoteMissingProb = homozygoteMissingProb;
	viterbi.heterozygoteMissingProb = heterozygoteMissingProb;
	viterbi.intercrossingSingleLociHaplotypeProbabilities = &intercrossingSingleLociHaplotypeProbabilities;
	viterbi.funnelSingleLociHaplotypeProbabilities = &funnelSingleLociHaplotypeProbabilities;

	//Now actually run the Viterbi algorithm. To cut down on memory usage we run a single chromosome at a time
	for(int chromosomeCounter = 0; chromosomeCounter < map.size(); chromosomeCounter++)
	{
		Rcpp::NumericVector positions = Rcpp::as<Rcpp::NumericVector>(map(chromosomeCounter));
		Rcpp::NumericVector recombinationFractions = haldaneToRf(diff(positions));
		//Generate haplotype probability data. 
		for(int markerCounter = 0; markerCounter < recombinationFractions.size(); markerCounter++)
		{
			double recombination = recombinationFractions(markerCounter);
			for(int selfingGenerationCounter = minSelfing; selfingGenerationCounter <= maxSelfing; selfingGenerationCounter++)
			{
				expandedGenotypeProbabilities<nFounders, infiniteSelfing, true>::noIntercross(funnelHaplotypeProbabilities(markerCounter, selfingGenerationCounter - minSelfing), recombination, selfingGenerationCounter, nFunnels);
			}
			for(int selfingGenerationCounter = minSelfing; selfingGenerationCounter <= maxSelfing; selfingGenerationCounter++)
			{
				for(int intercrossingGenerations =  minAIGenerations; intercrossingGenerations <= maxAIGenerations; intercrossingGenerations++)
				{
					expandedGenotypeProbabilities<nFounders, infiniteSelfing, true>::withIntercross(intercrossingHaplotypeProbabilities(markerCounter, intercrossingGenerations - minAIGenerations, selfingGenerationCounter - minSelfing), intercrossingGenerations, recombination, selfingGenerationCounter, nFunnels);
				}
			}

		}
		//dispatch based on whether we have infinite generations of selfing or not. 
		viterbi.apply(cumulativeMarkerCounter, cumulativeMarkerCounter+(int)positions.size());
		cumulativeMarkerCounter += (int)positions.size();
	}
}
template<int nFounders> void imputedFoundersInternal1(Rcpp::IntegerMatrix founders, Rcpp::IntegerMatrix finals, Rcpp::S4 pedigree, Rcpp::List hetData, Rcpp::List map, Rcpp::IntegerMatrix results, bool infiniteSelfing, double homozygoteMissingProb, double heterozygoteMissingProb, Rcpp::IntegerMatrix key)
{
	if(infiniteSelfing)
	{
		imputedFoundersInternal2<nFounders, true>(founders, finals, pedigree, hetData, map, results, homozygoteMissingProb, heterozygoteMissingProb, key);
	}
	else
	{
		imputedFoundersInternal2<nFounders, false>(founders, finals, pedigree, hetData, map, results, homozygoteMissingProb, heterozygoteMissingProb, key);
	}
}
SEXP imputeFounders(SEXP geneticData_sexp, SEXP map_sexp, SEXP homozygoteMissingProb_sexp, SEXP heterozygoteMissingProb_sexp)
{
BEGIN_RCPP
	Rcpp::S4 geneticData;
	try
	{
		geneticData = Rcpp::as<Rcpp::S4>(geneticData_sexp);
	}
	catch(...)
	{
		throw std::runtime_error("Input geneticData must be an S4 object of class geneticData");
	}

	Rcpp::IntegerMatrix founders;
	try
	{
		founders = Rcpp::as<Rcpp::IntegerMatrix>(geneticData.slot("founders"));
	}
	catch(...)
	{
		throw std::runtime_error("Input geneticData@founders must be an integer matrix");
	}

	Rcpp::IntegerMatrix finals;
	try
	{
		finals = Rcpp::as<Rcpp::IntegerMatrix>(geneticData.slot("finals"));
	}
	catch(...)
	{
		throw std::runtime_error("Input geneticData@finals must be an integer matrix");
	}

	Rcpp::S4 pedigree;
	try
	{
		pedigree = Rcpp::as<Rcpp::S4>(geneticData.slot("pedigree"));
	}
	catch(...)
	{
		throw std::runtime_error("Input geneticData@pedigree must be an S4 object");
	}

	std::string pedigreeSelfingSlot;
	try
	{
		pedigreeSelfingSlot = Rcpp::as<std::string>(pedigree.slot("selfing"));
	}
	catch(...)
	{
		throw std::runtime_error("Input geneticData@pedigree@selfing must be a string");
	}
	bool infiniteSelfing;
	if(pedigreeSelfingSlot == "infinite")
	{
		infiniteSelfing = true;
	}
	else if(pedigreeSelfingSlot == "finite")
	{
		infiniteSelfing = false;
	}
	else
	{
		throw std::runtime_error("Input geneticData@pedigree@selfing must be \"infinite\" or \"finite\"");
	}

	Rcpp::List hetData;
	try
	{
		hetData = Rcpp::as<Rcpp::List>(geneticData.slot("hetData"));
	}
	catch(...)
	{
		throw std::runtime_error("Input geneticData@hetData must be a list");
	}

	Rcpp::List map;
	try
	{
		map = Rcpp::as<Rcpp::List>(map_sexp);
	}
	catch(...)
	{
		throw std::runtime_error("Input map must be a list");
	}

	double homozygoteMissingProb;
	try
	{
		homozygoteMissingProb = Rcpp::as<double>(homozygoteMissingProb_sexp);
	}
	catch(...)
	{
		throw std::runtime_error("Input homozygoteMissingProb must be a number between 0 and 1");
	}
	if(homozygoteMissingProb < 0 || homozygoteMissingProb > 1) throw std::runtime_error("Input homozygoteMissingProb must be a number between 0 and 1");

	double heterozygoteMissingProb;
	try
	{
		heterozygoteMissingProb = Rcpp::as<double>(heterozygoteMissingProb_sexp);
	}
	catch(...)
	{
		throw std::runtime_error("Input heterozygoteMissingProb must be a number between 0 and 1");
	}
	if(heterozygoteMissingProb < 0 || heterozygoteMissingProb > 1) throw std::runtime_error("Input heterozygoteMissingProb must be a number between 0 and 1");

	std::vector<std::string> foundersMarkers = Rcpp::as<std::vector<std::string> >(Rcpp::colnames(founders));
	std::vector<std::string> finalsMarkers = Rcpp::as<std::vector<std::string> >(Rcpp::colnames(finals));
	std::vector<std::string> lineNames = Rcpp::as<std::vector<std::string> >(Rcpp::rownames(finals));

	Rcpp::Function nFoundersFunc("nFounders");
	int nFounders = Rcpp::as<int>(nFoundersFunc(geneticData));

	//Construct the key that takes pairs of founder values and turns them into encodings
	Rcpp::IntegerMatrix key(nFounders, nFounders);
	for(int i = 0; i < nFounders; i++)
	{
		key(i, i) = i + 1;
	}
	int counter = nFounders+1;
	for(int i = 0; i < nFounders; i++)
	{
		for(int j = i+1; j < nFounders; j++)
		{
			key(j, i) = key(i, j) = counter;
			counter++;
		}
	}
	//We also want a version closer to the hetData format
	Rcpp::IntegerMatrix outputKey(nFounders*nFounders, 3);
	{
		int counter = 0;
		for(int i = 0; i < nFounders; i++)
		{
			for(int j = 0; j < nFounders; j++)
			{
				outputKey(counter, 0) = i+1;
				outputKey(counter, 1) = j+1;
				outputKey(counter, 2) = key(i, j);
				counter++;
			}
		}
	}

	std::vector<std::string> mapMarkers;
	mapMarkers.reserve(foundersMarkers.size());
	int maxChromosomeMarkers = 0;
	for(int i = 0; i < map.size(); i++)
	{
		Rcpp::NumericVector chromosome;
		try
		{
			chromosome = Rcpp::as<Rcpp::NumericVector>(map(i));
		}
		catch(...)
		{
			throw std::runtime_error("Input map must be a list of numeric vectors");
		}
		Rcpp::CharacterVector chromosomeMarkers = chromosome.names();
		mapMarkers.insert(mapMarkers.end(), chromosomeMarkers.begin(), chromosomeMarkers.end());
		maxChromosomeMarkers = std::max(maxChromosomeMarkers, (int)chromosomeMarkers.size());
	}
	if(mapMarkers.size() != foundersMarkers.size() || !std::equal(mapMarkers.begin(), mapMarkers.end(), foundersMarkers.begin()))
	{
		throw std::runtime_error("Map was inconsistent with the markers in the geneticData object");
	}
	if(mapMarkers.size() != finalsMarkers.size() || !std::equal(mapMarkers.begin(), mapMarkers.end(), finalsMarkers.begin()))
	{
		throw std::runtime_error("Map was inconsistent with the markers in the geneticData object");
	}

	int nFinals = finals.nrow();
	Rcpp::IntegerMatrix results(nFinals, (int)mapMarkers.size());
	try
	{
		if(nFounders == 2)
		{
			imputedFoundersInternal1<2>(founders, finals, pedigree, hetData, map, results, infiniteSelfing, homozygoteMissingProb, heterozygoteMissingProb, key);
		}
		else if(nFounders == 4)
		{
			imputedFoundersInternal1<4>(founders, finals, pedigree, hetData, map, results, infiniteSelfing, homozygoteMissingProb, heterozygoteMissingProb, key);
		}
		else if(nFounders == 8)
		{
			imputedFoundersInternal1<8>(founders, finals, pedigree, hetData, map, results, infiniteSelfing, homozygoteMissingProb, heterozygoteMissingProb, key);
		}
		else if(nFounders == 16)
		{
			imputedFoundersInternal1<16>(founders, finals, pedigree, hetData, map, results, infiniteSelfing, homozygoteMissingProb, heterozygoteMissingProb, key);
		}
		else
		{
			throw std::runtime_error("Number of founders must be 2, 4, 8 or 16");
		}
	}
	catch(impossibleDataException err)
	{
		std::stringstream ss;
		ss << "Impossible data may have been detected for markers " << mapMarkers[err.marker] << " and " << mapMarkers[err.marker+1] << " for line " << lineNames[err.line] << ". Are these markers at the same location, and if so does this line have a recombination event between these markers?"; 
		throw std::runtime_error(ss.str().c_str());
	}
	return Rcpp::List::create(Rcpp::Named("data") = results, Rcpp::Named("key") = outputKey);
END_RCPP
}

