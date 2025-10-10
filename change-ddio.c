/* 
 * Changing DDIO State
 *
 * Copyright (c) 2020, Alireza Farshin, KTH Royal Institute of Technology - All Rights Reserved
 */

// Compile command: gcc change-ddio.c -o <binary_name> -lpci

#include <stdio.h>
#include <stdlib.h>
#include <pci/pci.h>
#include <sys/io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <inttypes.h>
#include <unistd.h>

#define PCI_VENDOR_ID_INTEL   	0x8086
#define SKX_PERFCTRLSTS_0	0x180
#define SKX_use_allocating_flow_wr_MASK 0x80
#define SKX_nosnoopopwren_MASK	0x8

/*
 * Find the proper pci device (i.e., PCIe Root Port) based on the nic device
 * For instance, if the NIC is located on 0000:17:00.0 (i.e., BDF)
 * 0x17 is the nic_bus (B)
 * 0x00 is the nic_device (D)
 * 0x0  is the nic_function (F)
 */

struct pci_access *pacc;

void
init_pci_access(void)
{
	pacc = pci_alloc();           /* Get the pci_access structure */
	pci_init(pacc);               /* Initialize the PCI library */
	pci_scan_bus(pacc);           /* We want to get the list of devices */
}

struct pci_dev*
find_ddio_device(uint8_t nic_bus)
{
	struct pci_dev* dev;
	struct pci_dev* best_match = NULL;
	uint8_t lowest_bus = 0xff;

	for(dev = pacc->devices; dev; dev=dev->next) {
		pci_fill_info(dev, PCI_FILL_IDENT | PCI_FILL_BASES |
		              PCI_FILL_NUMA_NODE | PCI_FILL_PHYS_SLOT | PCI_FILL_CLASS);
		/*
		 * Find the proper PCIe root based on the nic device
		 * For instance, if the NIC is located on 0000:17:00.0 (i.e., BDF)
		 * 0x17 is the nic_bus (B)
		 * 0x00 is the nic_device (D)
		 * 0x0	is the nic_function (F)
		 *
		 * We need to find the Root Port (closest to root complex) that covers nic_bus.
		 * Multiple bridges in a hierarchy may have the same subordinate bus,
		 * so we select the one with the lowest bus number.
		 */
		uint8_t subordinate = pci_read_byte(dev, PCI_SUBORDINATE_BUS);
		uint8_t secondary = pci_read_byte(dev, PCI_SECONDARY_BUS);

		// Check if this bridge covers the nic_bus range
		if (subordinate == nic_bus &&
		    secondary <= nic_bus &&
		    subordinate >= nic_bus) {

			// Select the bridge with the lowest bus number (closest to root)
			if (dev->bus < lowest_bus) {
				lowest_bus = dev->bus;
				best_match = dev;
			}
		}
	}

	if (!best_match) {
		printf("Could not find the proper PCIe root!\n");
	}
	return best_match;
}


/*
 * perfctrlsts_0 Register (Offset 0x180)
 *
 * Bit 7: Use_Allocating_Flow_Wr
 *   - 1b: DDIO enabled (direct cache injection for PCIe writes)
 *   - 0b: DDIO disabled (PCIe writes bypass LLC, go to memory)
 *
 * Bit 3: NoSnoopOpWrEn (NS enable/disable)
 *   - 1b: Non-Snoop (NS) writes enabled - PCIe writes go directly to memory
 *   - 0b: Non-Snoop (NS) writes disabled - PCIe writes go to LLC (last level cache)
 *
 * Reference: IntelÂ® XeonÂ® Processor Scalable Family
 * Datasheet, Volume Two: Registers (May 2019, p. 68)
 * link: https://www.intel.com/content/www/us/en/processors/xeon/scalable/xeon-scalable-datasheet-vol-2.html
 */
int
ddio_status(uint8_t nic_bus)
{
	uint32_t val;
	if(!pacc)
		init_pci_access();

	struct pci_dev* dev=find_ddio_device(nic_bus);
	if(!dev){
		printf("No device found!\n");
		exit(1);
	}
	val=pci_read_long(dev,SKX_PERFCTRLSTS_0);
	printf("perfctrlsts_0 val: 0x%" PRIx32 "\n",val);
	printf("NoSnoopOpWrEn val: 0x%" PRIx32 "\n",val&SKX_nosnoopopwren_MASK);
	printf("Use_Allocating_Flow_Wr val: 0x%" PRIx32 "\n",val&SKX_use_allocating_flow_wr_MASK);
	if(val&SKX_use_allocating_flow_wr_MASK)
		return 1;
	else
		return 0;
}

/*
 * Configure DDIO and NoSnoop settings
 *
 * Parameters:
 *   nic_bus: PCIe bus number of the NIC device
 *   use_allocating_flow_wr: DDIO enable (1) / disable (0)
 *   nosnoopopwren: NS enable for memory write (1) / NS disable for LLC write (0)
 */
void
ddio_configure(uint8_t nic_bus, uint8_t use_allocating_flow_wr, uint8_t nosnoopopwren)
{
	uint32_t val_before, val_after, val_new;
	if(!pacc)
		init_pci_access();

	struct pci_dev* dev=find_ddio_device(nic_bus);
	if(!dev){
		printf("No device found!\n");
		exit(1);
	}

	// Read current register value
	val_before = pci_read_long(dev, SKX_PERFCTRLSTS_0);
	printf("\n=== BEFORE Configuration ===\n");
	printf("perfctrlsts_0 register value: 0x%08" PRIx32 "\n", val_before);
	printf("  Use_Allocating_Flow_Wr (bit 7, DDIO): 0x%02" PRIx32 " (%s)\n",
	       (val_before & SKX_use_allocating_flow_wr_MASK) >> 7,
	       (val_before & SKX_use_allocating_flow_wr_MASK) ? "enabled" : "disabled");
	printf("  NoSnoopOpWrEn (bit 3, NS): 0x%02" PRIx32 " (%s)\n",
	       (val_before & SKX_nosnoopopwren_MASK) >> 3,
	       (val_before & SKX_nosnoopopwren_MASK) ? "mem write" : "LLC write");

	// Calculate new value
	val_new = val_before;

	// Set or clear Use_Allocating_Flow_Wr bit (bit 7)
	if (use_allocating_flow_wr) {
		val_new |= SKX_use_allocating_flow_wr_MASK;  // Set bit 7 (DDIO enable)
	} else {
		val_new &= ~SKX_use_allocating_flow_wr_MASK; // Clear bit 7 (DDIO disable)
	}

	// Set or clear NoSnoopOpWrEn bit (bit 3)
	if (nosnoopopwren) {
		val_new |= SKX_nosnoopopwren_MASK;  // Set bit 3 (NS enable - mem write)
	} else {
		val_new &= ~SKX_nosnoopopwren_MASK; // Clear bit 3 (NS disable - LLC write)
	}

	// Write new value
	pci_write_long(dev, SKX_PERFCTRLSTS_0, val_new);

	// Read back to verify
	val_after = pci_read_long(dev, SKX_PERFCTRLSTS_0);
	printf("\n=== AFTER Configuration ===\n");
	printf("perfctrlsts_0 register value: 0x%08" PRIx32 "\n", val_after);
	printf("  Use_Allocating_Flow_Wr (bit 7, DDIO): 0x%02" PRIx32 " (%s)\n",
	       (val_after & SKX_use_allocating_flow_wr_MASK) >> 7,
	       (val_after & SKX_use_allocating_flow_wr_MASK) ? "enabled" : "disabled");
	printf("  NoSnoopOpWrEn (bit 3, NS): 0x%02" PRIx32 " (%s)\n",
	       (val_after & SKX_nosnoopopwren_MASK) >> 3,
	       (val_after & SKX_nosnoopopwren_MASK) ? "mem write" : "LLC write");

	printf("\nConfiguration applied successfully!\n");
}

void
print_dev_info(struct pci_dev *dev)
{
	if(!dev){
		printf("No device found!\n");
		exit(1);
        }
	unsigned int c;
	char namebuf[1024], *name;
	printf("========================\n");
	printf("%04x:%02x:%02x.%d vendor=%04x device=%04x class=%04x irq=%d (pin %d) base0=%lx \n",
                        dev->domain, dev->bus, dev->dev, dev->func, dev->vendor_id, dev->device_id,
                        dev->device_class, dev->irq, c, (long) dev->base_addr[0]);
	name = pci_lookup_name(pacc, namebuf, sizeof(namebuf), PCI_LOOKUP_DEVICE, dev->vendor_id, dev->device_id);
	printf(" (%s)\n", name);
	printf("========================\n");
}

int main(int argc, char *argv[])
{
  // Check for required command-line arguments
  if (argc != 4) {
    printf("Usage: %s <port_num> <use_allocating_flow_wr> <nosnoopopwren>\n", argv[0]);
    printf("\nArguments:\n");
    printf("  port_num              : End device port number (hex, e.g., 0x9b or decimal)\n");
    printf("  use_allocating_flow_wr: DDIO enable (1) / disable (0)\n");
    printf("  nosnoopopwren         : NS enable for mem write (1) / NS disable for LLC write (0)\n");
    printf("\nExample:\n");
    printf("  %s 0x9b 1 0    # Enable DDIO, disable NS (LLC write)\n", argv[0]);
    printf("  %s 155 0 1     # Disable DDIO, enable NS (mem write)\n", argv[0]);
    return 1;
  }

  // Parse command-line arguments
  uint8_t nic_bus = (uint8_t)strtol(argv[1], NULL, 0);  // Supports both hex (0x9b) and decimal (155)
  uint8_t use_allocating_flow_wr = (uint8_t)atoi(argv[2]);
  uint8_t nosnoopopwren = (uint8_t)atoi(argv[3]);

  // Validate bit arguments
  if (use_allocating_flow_wr > 1 || nosnoopopwren > 1) {
    printf("Error: use_allocating_flow_wr and nosnoopopwren must be 0 or 1\n");
    return 1;
  }

  printf("Configuration parameters:\n");
  printf("  Port number (nic_bus): 0x%02x (%d)\n", nic_bus, nic_bus);
  printf("  Use_Allocating_Flow_Wr (DDIO): %d (%s)\n",
         use_allocating_flow_wr, use_allocating_flow_wr ? "enable" : "disable");
  printf("  NoSnoopOpWrEn (NS): %d (%s)\n",
         nosnoopopwren, nosnoopopwren ? "mem write" : "LLC write");

  init_pci_access();

  struct pci_dev *dev = find_ddio_device(nic_bus);
  print_dev_info(dev);

  // Configure DDIO and NoSnoop settings
  ddio_configure(nic_bus, use_allocating_flow_wr, nosnoopopwren);

  pci_cleanup(pacc);		/* Close everything */
  return 0;
}
