/*

miniAD is a miniapp of the GPU version of AutoDock 4.2 running a Lamarckian Genetic Algorithm
Copyright (C) 2017 TU Darmstadt, Embedded Systems and Applications Group, Germany. All rights reserved.
For some of the code, Copyright (C) 2019 Computational Structural Biology Center, the Scripps Research Institute.

AutoDock is a Trade Mark of the Scripps Research Institute.

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

*/


// if defined, new (experimental) SW genotype moves that are dependent
// on nr of atoms and nr of torsions of ligand are used
#define SWAT3 // Third set of Solis-Wets hyperparameters by Andreas Tillack

void gpu_perform_LS( uint32_t nblocks, uint32_t nthreads, float* pMem_conformations_next, float* pMem_energies_next )

//The GPU global function performs local search on the pre-defined entities of conformations_next.
//The number of blocks which should be started equals to num_of_lsentities*num_of_runs.
//This way the first num_of_lsentities entity of each population will be subjected to local search
//(and each block carries out the algorithm for one entity).
//Since the first entity is always the best one in the current population,
//it is always tested according to the ls probability, and if it not to be
//subjected to local search, the entity with ID num_of_lsentities is selected instead of the first one (with ID 0).
{

    #pragma omp target 
    #pragma omp teams distribute num_teams(nblocks) thread_limit(nthreads)
    for (int blockIdx = 0; blockIdx < nblocks; blockIdx++){	
         float genotype_candidate[ACTUAL_GENOTYPE_LENGTH];
         float genotype_deviate  [ACTUAL_GENOTYPE_LENGTH];
         float genotype_bias     [ACTUAL_GENOTYPE_LENGTH];
         float rho;
         int   cons_succ;
         int   cons_fail;
         int   iteration_cnt;
         int   evaluation_cnt;
         float3 calc_coords[MAX_NUM_OF_ATOMS];
         float offspring_genotype[ACTUAL_GENOTYPE_LENGTH];
         float offspring_energy;
         float sFloatAccumulator;
         int entity_id;
         size_t scratchpad = MAX_NUM_OF_ATOMS + 4*ACTUAL_GENOTYPE_LENGTH;
         #pragma omp parallel for\
              private(scratchpad)\
              allocator(omp_pteam_memalloc)
         for (int idx = 0; idx < nthreads; idx++){
              float candidate_energy;
              int run_id;

	// Determining run ID and entity ID
	// Initializing offspring genotype
              run_id = blockIdx / cData.dockpars.num_of_lsentities;
	      if (idx == 0)
	      {
                  entity_id = blockIdx % cData.dockpars.num_of_lsentities;

		// Since entity 0 is the best one due to elitism,
		// it should be subjected to random selection
		if (entity_id == 0) {
			// If entity 0 is not selected according to LS-rate,
			// choosing an other entity
			if (100.0f*gpu_randf(cData.pMem_prng_states) > cData.dockpars.lsearch_rate) {
				entity_id = cData.dockpars.num_of_lsentities;					
			}
		}

		offspring_energy = pMem_energies_next[run_id*cData.dockpars.pop_size+entity_id];
		rho = 1.0f;
		cons_succ = 0;
		cons_fail = 0;
		iteration_cnt = 0;
		evaluation_cnt = 0;        
	      }
    __threadfence();
    __syncthreads();

    size_t offset = (run_id * cData.dockpars.pop_size + entity_id) * GENOTYPE_LENGTH_IN_GLOBMEM;
	for (uint32_t gene_counter = idx;
	     gene_counter < cData.dockpars.num_of_genes;
	     gene_counter+= blockDim.x) {
        offspring_genotype[gene_counter] = pMem_conformations_next[offset + gene_counter];
		genotype_bias[gene_counter] = 0.0f;
	}
    __threadfence();
	__syncthreads();
    

#ifdef SWAT3
	float lig_scale = 1.0f/sqrt((float)cData.dockpars.num_of_atoms);
	float gene_scale = 1.0f/sqrt((float)cData.dockpars.num_of_genes);
#endif
	while ((iteration_cnt < cData.dockpars.max_num_of_iters) && (rho > cData.dockpars.rho_lower_bound))
	{
		// New random deviate
		for (uint32_t gene_counter = idx;
		     gene_counter < cData.dockpars.num_of_genes;
		     gene_counter+= blockDim.x)
		{
#ifdef SWAT3
			genotype_deviate[gene_counter] = rho*(2*gpu_randf(cData.pMem_prng_states)-1)*(gpu_randf(cData.pMem_prng_states) < gene_scale);

			// Translation genes
			if (gene_counter < 3) {
				genotype_deviate[gene_counter] *= cData.dockpars.base_dmov_mul_sqrt3;
			}
			// Orientation and torsion genes
			else {
				if (gene_counter < 6) {
					genotype_deviate[gene_counter] *= cData.dockpars.base_dang_mul_sqrt3 * lig_scale;
				} else {
					genotype_deviate[gene_counter] *= cData.dockpars.base_dang_mul_sqrt3 * gene_scale;
				}
			}
#else
			genotype_deviate[gene_counter] = rho*(2*gpu_randf(cData.pMem_prng_states)-1)*(gpu_randf(cData.pMem_prng_states)<0.3f);

			// Translation genes
			if (gene_counter < 3) {
				genotype_deviate[gene_counter] *= cData.dockpars.base_dmov_mul_sqrt3;
			}
			// Orientation and torsion genes
			else {
				genotype_deviate[gene_counter] *= cData.dockpars.base_dang_mul_sqrt3;
			}
#endif
		}

		// Generating new genotype candidate
		for (uint32_t gene_counter = idx;
		     gene_counter < cData.dockpars.num_of_genes;
		     gene_counter+= blockDim.x) {
			   genotype_candidate[gene_counter] = offspring_genotype[gene_counter] + 
							      genotype_deviate[gene_counter]   + 
							      genotype_bias[gene_counter];
		}

		// Evaluating candidate
        __threadfence();
        __syncthreads();

		// ==================================================================
		gpu_calc_energy(
                genotype_candidate,
                candidate_energy,
                run_id,
                calc_coords,
                &sFloatAccumulator
				);
		// =================================================================

		if (idx == 0) {
			evaluation_cnt++;
		}
        __threadfence();
        __syncthreads();

		if (candidate_energy < offspring_energy)	// If candidate is better, success
		{
			for (uint32_t gene_counter = idx;
			     gene_counter < cData.dockpars.num_of_genes;
			     gene_counter+= blockDim.x)
			{
				// Updating offspring_genotype
				offspring_genotype[gene_counter] = genotype_candidate[gene_counter];

				// Updating genotype_bias
				genotype_bias[gene_counter] = 0.6f*genotype_bias[gene_counter] + 0.4f*genotype_deviate[gene_counter];
			}

			// Work-item 0 will overwrite the shared variables
			// used in the previous if condition
			__threadfence();
            __syncthreads();

			if (idx == 0)
			{
				offspring_energy = candidate_energy;
				cons_succ++;
				cons_fail = 0;
			}
		}
		else	// If candidate is worser, check the opposite direction
		{
			// Generating the other genotype candidate
			for (uint32_t gene_counter = idx;
			     gene_counter < cData.dockpars.num_of_genes;
			     gene_counter+= blockDim.x) {
				   genotype_candidate[gene_counter] = offspring_genotype[gene_counter] - 
								      genotype_deviate[gene_counter] - 
								      genotype_bias[gene_counter];
			}

			// Evaluating candidate
			__threadfence();
            __syncthreads();

			// =================================================================
			gpu_calc_energy(
                genotype_candidate,
                candidate_energy,
                run_id,
                calc_coords,
                &sFloatAccumulator
            );
			// =================================================================

			if (idx == 0) {
				evaluation_cnt++;

				#if defined (DEBUG_ENERGY_KERNEL)
				printf("%-18s [%-5s]---{%-5s}   [%-10.8f]---{%-10.8f}\n", "-ENERGY-KERNEL3-", "GRIDS", "INTRA", partial_interE[0], partial_intraE[0]);
				#endif
			}
            __threadfence();
            __syncthreads();

			if (candidate_energy < offspring_energy) // If candidate is better, success
			{
				for (uint32_t gene_counter = idx;
				     gene_counter < cData.dockpars.num_of_genes;
			       	     gene_counter+= blockDim.x)
				{
					// Updating offspring_genotype
					offspring_genotype[gene_counter] = genotype_candidate[gene_counter];

					// Updating genotype_bias
					genotype_bias[gene_counter] = 0.6f*genotype_bias[gene_counter] - 0.4f*genotype_deviate[gene_counter];
				}

				// Work-item 0 will overwrite the shared variables
				// used in the previous if condition
                __threadfence();
                __syncthreads();

				if (idx == 0)
				{
					offspring_energy = candidate_energy;
					cons_succ++;
					cons_fail = 0;
				}
			}
			else	// Failure in both directions
			{
				for (uint32_t gene_counter = idx;
				     gene_counter < cData.dockpars.num_of_genes;
				     gene_counter+= blockDim.x)
					   // Updating genotype_bias
					   genotype_bias[gene_counter] = 0.5f*genotype_bias[gene_counter];

				if (idx == 0)
				{
					cons_succ = 0;
					cons_fail++;
				}
			}
		}

		// Changing rho if needed
		if (idx == 0)
		{
			iteration_cnt++;

			if (cons_succ >= cData.dockpars.cons_limit)
			{
				rho *= LS_EXP_FACTOR;
				cons_succ = 0;
			}
			else
				if (cons_fail >= cData.dockpars.cons_limit)
				{
					rho *= LS_CONT_FACTOR;
					cons_fail = 0;
				}
		}
        __threadfence();
        __syncthreads();
	}

	// Updating eval counter and energy
	if (idx == 0) {
		cData.pMem_evals_of_new_entities[run_id*cData.dockpars.pop_size+entity_id] += evaluation_cnt;
		pMem_energies_next[run_id*cData.dockpars.pop_size+entity_id] = offspring_energy;
	}

	// Mapping torsion angles and writing out results
    offset = (run_id*cData.dockpars.pop_size+entity_id)*GENOTYPE_LENGTH_IN_GLOBMEM;
	for (uint32_t gene_counter = idx;
	     gene_counter < cData.dockpars.num_of_genes;
	     gene_counter+= blockDim.x) {
        if (gene_counter >= 3) {
		    map_angle(offspring_genotype[gene_counter]);
		}
        pMem_conformations_next[offset + gene_counter] = offspring_genotype[gene_counter];
	}

    }
}

