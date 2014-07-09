/**
 * \file logger.c
 * \brief Logs incoming UniRec records into file(s).
 * \author Vaclav Bartos <ibartosv@fit.vutbr.cz>
 * \author Erik Sabik <xsabik02@stud.fit.vutbr.cz>
 * \author Katerina Pilatova <xpilat05@stud.fit.vutbr.cz>
 * \date 2013
 * \date 2014
 */
/*
 * Copyright (C) 2013,2014 CESNET
 *
 * LICENSE TERMS
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of the Company nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * ALTERNATIVELY, provided that this notice is retained in full, this
 * product may be distributed under the terms of the GNU General Public
 * License (GPL) version 2 or later, in which case the provisions
 * of the GPL apply INSTEAD OF those given above.
 *
 * This software is provided ``as is'', and any express or implied
 * warranties, including, but not limited to, the implied warranties of
 * merchantability and fitness for a particular purpose are disclaimed.
 * In no event shall the company or contributors be liable for any
 * direct, indirect, incidental, special, exemplary, or consequential
 * damages (including, but not limited to, procurement of substitute
 * goods or services; loss of use, data, or profits; or business
 * interruption) however caused and on any theory of liability, whether
 * in contract, strict liability, or tort (including negligence or
 * otherwise) arising in any way out of the use of this software, even
 * if advised of the possibility of such damage.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <getopt.h>
#include <time.h>
#include <libtrap/trap.h>
#include <unirec/unirec.h>
#include <omp.h>

// Struct with information about module
trap_module_info_t module_info = {
   "Logger", // Module name
   // Module description
   "This module logs all incoming UniRec records to standard output or into a\n"
   "specified file. Each record is written as one line containing values of its\n"
   "fields in human-readable format separated by chosen delimiters (CSV format).\n"
   "Number of input intefaces and their UniRec formats are given on command line\n"
   "(if you specify N UniRec formats, N input interfaces will be created).\n"
   "Output contains union of all fields of all input formats by default, but it may\n"
   "be redefined using -o option.\n"
   "\n"
   "Interfaces:\n"
   "   Inputs: variable\n"
   "   Outputs: 0\n"
   "\n"
   "Usage:\n"
   "   ./logger -i IFC_SPEC [-w|-a FILE] UNIREC_FMT [UNIREC_FMT ...] [-o OUT_FMT] [-t] [-n] [-c N] [-d X]\n"
   "\n"
   "Module specific parameters:\n"
   "   UNIREC_FMT   The i-th parameter of this type specifies format of UniRec\n"
   "                expected on the i-th input interface.\n"
   "   -w FILE      Write output to FILE instead of stdout (rewrite the file).\n"
   "   -a FILE      Write output to FILE instead of stdout (append to the end).\n"
   "   -o OUT_FMT   Set of fields included in the output (UniRec specifier).\n"
   "                Union of all input formats is used by default.\n"
   "   -t           Write names of fields on the first line.\n"
   "   -T           Add the time when the record was received as the first field.\n"
   "   -n           Add the number of interface the record was received on as the\n"
   "                first field (or second when -T is specified).\n"
   "   -c N         Quit after N records are received.\n"
   "   -d X         Optionally modifies delimiter to inserted value X (implicitely ','). \n",
   -1, // Number of input interfaces (-1 means variable)
   0, // Number of output interfaces
};

static int stop = 0;

int verbose;
static int n_inputs; // Number of input interfaces
static ur_template_t **templates; // UniRec templates of input interfaces (array of length n_inputs)
static ur_template_t *out_template; // UniRec template with union of fields of all inputs
int print_ifc_num = 0;
int print_time = 0;

unsigned int num_records = 0; // Number of records received (total of all inputs)
unsigned int max_num_records = 0; // Exit after this number of records is received


static FILE *file; // Output file

TRAP_DEFAULT_SIGNAL_HANDLER(stop = 1);

void capture_thread(int index, char *delimiter)
{
   int ret;

   if (verbose >= 1) {
      printf("Thread %i started.\n", index);
   }

   // Read data from input and log them to a file
   while (!stop) {
      const void *rec;
      uint16_t rec_size;

      if (verbose >= 2) {
         printf("Thread %i: calling trap_recv()\n", index);
      }

      // Receive data from index-th input interface, wait until data are available
      ret = trap_recv(index, &rec, &rec_size);
      TRAP_DEFAULT_RECV_ERROR_HANDLING(ret, continue, break);

      if (verbose >= 2) {
         printf("Thread %i: received %hu bytes of data\n", index, rec_size);
      }

      // Check size of received data
      if (rec_size < ur_rec_static_size(templates[index])) {
         if (rec_size <= 1) {
            if (verbose >= 0) {
               printf("Interface %i received ending record, the interface will be closed.\n", index, rec_size);
            }
            break; // End of data (used for testing purposes)
         } else {
            fprintf(stderr, "Error: data with wrong size received (expected size: >= %hu, received size: %hu)\n",
                    ur_rec_static_size(templates[index]), rec_size);
            break;
         }
      }

      // Print contents of received UniRec to output
      #pragma omp critical
      {
         if (print_time) {
            char str[32];
            time_t ts = time(NULL);
            strftime(str, 31, "%FT%T", gmtime(&ts));
            fprintf(file, "%s,", str);
         }
         if (print_ifc_num) {
            fprintf(file,"%i,", index);
         }
         // Iterate over all output fields
         int indent = 0;
         ur_field_id_t id;
         ur_iter_t iter = UR_ITER_BEGIN;
         while((id = ur_iter_fields_tmplt(out_template, &iter)) != UR_INVALID_FIELD) {
            if (indent) {
               fprintf(file,"%s", delimiter);
            }
            indent = 1;
            if (ur_is_present(templates[index], id)) {
               // Get pointer to the field (valid for static fields only)
               void *ptr = ur_get_ptr_by_id(templates[index], rec, id);
               // Print the field
               if (id == UR_TIMESLOT) {
                  char str[32];
                  time_t ts = *(uint32_t*)ptr;
                  strftime(str, 31, "%FT%T", gmtime(&ts));
                  fprintf(file, "%s", str);
               } else {
                  // Static field - check what type is it and use appropriate format
                  switch (ur_get_type_by_id(id)) {
                  case UR_TYPE_UINT8:
                     fprintf(file, "%u", *(uint8_t*)ptr);
                     break;
                  case UR_TYPE_UINT16:
                     fprintf(file, "%u", *(uint16_t*)ptr);
                     break;
                  case UR_TYPE_UINT32:
                     fprintf(file, "%u", *(uint32_t*)ptr);
                     break;
                  case UR_TYPE_UINT64:
                     fprintf(file, "%lu", *(uint64_t*)ptr);
                     break;
                  case UR_TYPE_INT8:
                     fprintf(file, "%i", *(int8_t*)ptr);
                     break;
                  case UR_TYPE_INT16:
                     fprintf(file, "%i", *(int16_t*)ptr);
                     break;
                  case UR_TYPE_INT32:
                     fprintf(file, "%i", *(int32_t*)ptr);
                     break;
                  case UR_TYPE_INT64:
                     fprintf(file, "%li", *(int64_t*)ptr);
                     break;
                  case UR_TYPE_CHAR:
                     fprintf(file, "%c", *(char*)ptr);
                     break;
                  case UR_TYPE_FLOAT:
                     fprintf(file, "%f", *(float*)ptr);
                     break;
                  case UR_TYPE_DOUBLE:
                     fprintf(file, "%f", *(double*)ptr);
                     break;
                  case UR_TYPE_IP:
                     {
                        // IP address - convert to human-readable format and print
                        char str[46];
                        ip_to_str((ip_addr_t*)ptr, str);
                        fprintf(file, "%s", str);
                     }
                     break;
                  case UR_TYPE_TIME:
                     {
                        // Timestamp - convert to human-readable format and print
                        time_t sec = ur_time_get_sec(*(ur_time_t*)ptr);
                        int msec = ur_time_get_msec(*(ur_time_t*)ptr);
                        char str[32];
                        strftime(str, 31, "%FT%T", gmtime(&sec));
                        fprintf(file, "%s.%03i", str, msec);
                     }
                     break;
                  case UR_TYPE_STRING:
                     {
                        // Printable string - print it as it is
                        int size = ur_get_dyn_size(templates[index], rec, id);
                        char *data = ur_get_dyn(templates[index], rec, id);
                        fprintf(file, "%*s", size, data);
                     }
                     break;
                  case UR_TYPE_BYTES:
                     {
                        // Generic string of bytes - print each byte as two hex digits
                        int size = ur_get_dyn_size(templates[index], rec, id);
                        unsigned char *data = ur_get_dyn(templates[index], rec, id);
                        while (size--) {
                           fprintf(file, "%02x", *data++);
                        }
                     }
                     break;
                  default:
                     {
                        // Unknown type - print the value in hex
                        int size = ur_get_size_by_id(id);
                        fprintf(file, "0x");
                        for (int i = 0; i < size; i++) {
                           fprintf(file, "%02x", ((unsigned char*)ptr)[i]);
                        }
                     }
                     break;
                  } // case (field type)
               } // if (not) dynamic
            } // if present
         } // loop over fields
         fprintf(file,"\n");
         fflush(file);

         num_records++;
      } // end critical section

      // Check whether maximum number of records has been reached
      if (max_num_records && num_records >= max_num_records) {
         stop = 1;
         trap_terminate();
         break;
      }
   } // end while(!stop)

   if (verbose >= 1) {
      printf("Thread %i exitting.\n", index);
   }
}


int main(int argc, char **argv)
{
   int ret;
   char *out_template_str = NULL;
   char *out_filename = NULL;
   int append = 0;
   int print_title = 0;
   char *delimiter = ",";

   // ***** Process parameters *****

   // Let TRAP library parse command-line arguments and extract its parameters
   trap_ifc_spec_t ifc_spec;
   ret = trap_parse_params(&argc, argv, &ifc_spec);
   if (ret != TRAP_E_OK) {
      if (ret == TRAP_E_HELP) { // "-h" was found
         trap_print_help(&module_info);
         return 0;
      }
      fprintf(stderr, "ERROR in parsing of parameters for TRAP: %s\n", trap_last_error_msg);
      return 1;
   }

   verbose = trap_get_verbose_level();
   if (verbose >= 0){
      printf("Verbosity level: %i\n", trap_get_verbose_level());
   }

   // Parse remaining parameters and get configuration
   char opt;
   while ((opt = getopt(argc, argv, "w:a:o:c:d:tnT")) != -1) {
      switch (opt) {
      case 'a':
         append = 1;
         // continue below...
      case 'w':
         if (out_filename != NULL) { // Output file is already specified
            fprintf(stderr, "Error: Only one output file may be specified.\n");
            return 1;
         }
         out_filename = optarg;
         break;
      case 'o':
         out_template_str = optarg;
         break;
      case 't':
         print_title = 1;
         break;
      case 'n':
         print_ifc_num = 1;
         break;
      case 'T':
         print_time = 1;
         break;
      case 'c':
         max_num_records = atoi(optarg);
         if (max_num_records == 0) {
            fprintf(stderr, "Error: Parameter of -c option must be integer > 0.\n");
            return 1;
         }
         break;
      case 'd':
         delimiter = optarg;
         if (strlen(delimiter) != 1) {
            fprintf(stderr, "Error: Parameter of -d option must contain 1 character.\n");
            return 1;
         }
         break;
      default:
         fprintf(stderr, "Error: Invalid arguments.\n");
         return 1;
      }
   }

   // Create UniRec templates
   n_inputs = argc - optind;
   if (verbose >= 0) {
      printf("Number of inputs: %i\n", n_inputs);
   }
   if (n_inputs < 1) {
      fprintf(stderr, "Error: You must specify at least one UniRec template.\n");
      return 0;
   }
   if (n_inputs > 32) {
      fprintf(stderr, "Error: More than 32 interfaces is not allowed by TRAP library.\n");
      return 4;
   }

   if (verbose >= 0) {
      printf("Creating UniRec templates ...\n");
   }
   templates = malloc(n_inputs*sizeof(*templates));
   if (templates == NULL) {
      fprintf(stderr, "Memory allocation error.\n");
      return -1;
   }

   for (int i = 0; i < n_inputs; i++) {
      templates[i] = ur_create_template(argv[i+optind]);
      if (templates[i] == NULL) {
         fprintf(stderr, "Error: Invalid template: %s\n", argv[i+optind]);
         while (i--) {
            ur_free_template(templates[i]);
         }
         free(templates);
         return 2;
      }
   }

   // Create output UniRec template (user-specified or union of all inputs)
   if (out_template_str == NULL) {
      // Create output template as a union of all input templates
      out_template = ur_union_templates(templates, n_inputs);
      if (out_template == NULL) {
         fprintf(stderr, "Memory allocation error\n");
         ret = -1;
         goto exit;
      }
   } else {
      out_template = ur_create_template(out_template_str);
      if (out_template == NULL) {
         fprintf(stderr, "Error: Invalid template: %s\n", out_template_str);
         ret = -1;
         goto exit;
      }
   }


   // ***** TRAP initialization *****

   // Set number of input interfaces
   module_info.num_ifc_in = n_inputs;

   if (verbose >= 0) {
      printf("Initializing TRAP library ...\n");
   }

   // Initialize TRAP library (create and init all interfaces)
   ret = trap_init(&module_info, ifc_spec);
   if (ret != TRAP_E_OK) {
      fprintf(stderr, "ERROR in TRAP initialization: %s\n", trap_last_error_msg);
      trap_free_ifc_spec(ifc_spec);
      ret = 2;
      goto exit;
   }

   // We don't need ifc_spec anymore, destroy it
   trap_free_ifc_spec(ifc_spec);

   // Register signal handler.
   TRAP_REGISTER_DEFAULT_SIGNAL_HANDLER();


   // ***** Open output file *****

   // Open output file if specified
   if (out_filename != NULL) {
      if (verbose >= 0) {
         printf("Creating output file \"%s\" ...\n", out_filename);
      }
      if (append) {
         file = fopen(out_filename, "a");
      } else {
         file = fopen(out_filename, "w");
      }
      if (file == NULL) {
         perror("Error: can't open output file:");
         ret = 3;
         goto exit;
      }
   } else {
      file = stdout;
   }

   if (verbose >= 0) {
      printf("Initialization done.\n");
   }

   // Print a header - names of output UniRec fields
   if (print_title) {
      if (print_time) {
         fprintf(file, "time,");
      }
      if (print_ifc_num) {
         fprintf(file, "ifc,");
      }
      int indent = 0;
      ur_field_id_t id;
      ur_iter_t iter = UR_ITER_BEGIN;
      while((id = ur_iter_fields_tmplt(out_template, &iter)) != UR_INVALID_FIELD) {
         if (indent) {
            fprintf(file, "%s", delimiter);
         }
         fprintf(file, "%s", ur_get_name_by_id(id));
         indent = 1;
      }
      fprintf(file, "\n");
      fflush(file);
   }

   // ***** Start a thread for each interface *****

   #pragma omp parallel num_threads(n_inputs)
   {
      capture_thread(omp_get_thread_num(), delimiter);
   }

   ret = 0;

   // ***** Cleanup *****

exit:
   if (verbose >= 0) {
      printf("Exitting ...\n");
   }

   trap_terminate(); // This have to be called before trap_finalize(), otherwise it may crash (don't know if feature or bug in TRAP)

   // Do all necessary cleanup before exiting
   TRAP_DEFAULT_FINALIZATION();

   for (int i = 0; i < n_inputs; i++) {
      ur_free_template(templates[i]);
   }
   free(templates);
   ur_free_template(out_template);

   return ret;
}

