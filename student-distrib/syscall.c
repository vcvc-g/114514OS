#include "syscall.h"

#define MAGIC_7F        0x7F
#define MAGIC_E         0x45
#define MAGIC_L         0x4C
#define MAGIC_F         0x46

#define BYTE_LEN           4
#define START_ADDR        24
#define ADDR_OFFSET        8

#define KSTACK_START        0x800000
#define PROGRAM_IMAGE_ADDR  0x8048000
#define PROC_NUM            6

#define ASM                 1
#define RESERV_FILES        2
#define ERR_BUF            26

#define FILE_RTC            0
#define FILE_DIR            1
#define FILE_REG            2


/* Current PID */
static int curr_process = 0;

/* Table of active and inactive processes (active = 1, inactive = 0) */
static int proc_state[PROC_NUM] = {0, 0, 0, 0, 0, 0};

/* Buffer for arguments and size storage */
static uint8_t argBuf[CMD_LIMIT];
static uint32_t argSize = 0;


/*
* set_curr_process
*   DESCRIPTION: Updates the current running process
*
*   INPUTS: int pid - new process ID
*   OUTPUTS: none
*   RETURN VALUE: none
*/
void set_curr_process(int pid) {
    curr_process = pid;
}


/*
* curr_running
*   DESCRIPTION: Checks if process at requested PID is currenlty running
*
*   INPUTS: int pid - process ID
*   OUTPUTS: 1 if process is running, 0 if not, or -1 on failure
*   RETURN VALUE: int values described above
*/
int8_t curr_running(int pid) {
    if (pid > 6 || pid < 0) { return -1;}
    return proc_state[pid];
}

/*
* pcb_init
*   DESCRIPTION: Initializes a new process control block for a process given
*       a process identification number (PID).
*
*   INPUTS: int pid - process identification number for process to be initialized
*   OUTPUTS: none
*   RETURN VALUE: none
*	SIDE EFFECTS : Creates a new PCB for the process with requested PID
*/
void pcb_init(int pid, int terminal_num, int isTerm) {
    int i;
    curr_process = pid;
    
    /* Create PCB for current process and assign PID */
    pcb_t* pcb= (pcb_t *)(KSTACK_BOT - PCB_SIZE * pid);
    pcb->pid = pid;

    /* Fill in file descriptors for reserved file stdin for every new process */
    pcb->file_array[0].read  = terminal_read_wrap;  //init stdin and stdout
    pcb->file_array[0].write = terminal_wrong;
    pcb->file_array[0].open  = terminal_nothing;
    pcb->file_array[0].close = terminal_nothing;
    pcb->file_array[0].flag  = 1;
    pcb->bitmap[0] = 1;
    /* Fill in file descriptors for reserved file stdout for every new process */
    pcb->file_array[1].read  = terminal_wrong;
    pcb->file_array[1].write = terminal_write_wrap;
    pcb->file_array[1].open  = terminal_nothing;
    pcb->file_array[1].close = terminal_nothing;
    pcb->file_array[1].flag  = 1;
    pcb->bitmap[1] = 1;

    pcb->current_ebp = KSTACK_START - (PCB_SIZE * pid) - MEM_FENCE;
    pcb->current_esp = KSTACK_START - (PCB_SIZE * pid) - MEM_FENCE;


    /* Set remaining files as unused (flag = 0) for every new process */
    for (i = RESERV_FILES; i < FDESC_SIZE; i++) {
        pcb->file_array[i].flag = 0;
    }


    // /* Check if this is the first process, and if it is, set parent ptr to NULL */
    // if (pid == 0) {
    //     pcb->p_pid = pid;
    // }
    // else {
    //     pcb->p_pid = pid-1;
    // }

    pcb_t* prev_pcb;
    // /* Upate TSS ss0 and esp0 */
    // if(curr_process == 0){
    //     prev_pcb=NULL;
    // }
    // else{
    //     prev_pcb=(pcb_t *)(KSTACK_BOT - PCB_SIZE * (pid-1));
    // }

    /* Check if we are initializing a PCB for a teriminal */
    if (isTerm) {
        prev_pcb = NULL;
        pcb->p_pid = pid;
        pcb->c_pid = -1;        /* No child set yet */

        pcb->parent_ebp = (KSTACK_START - PCB_SIZE * pid - MEM_FENCE);
        pcb->parent_esp = (KSTACK_START - PCB_SIZE * pid - MEM_FENCE);
    } else {
        /* Find PCB for current terminal and set its child PID */
        prev_pcb = get_term_pcb(terminal_num);
        prev_pcb->c_pid = pid;
        pcb->p_pid = prev_pcb->pid;

        pcb->parent_ebp = prev_pcb->current_ebp;
        pcb->parent_esp = prev_pcb->current_esp;
    }

    /* Save terminal num for prgrm or terminal to run */
    pcb->terminal = terminal_num;
    pcb->isTerm = isTerm;
    
    // if (isTerm == 0){
    //     prev_pcb->c_pid = pcb->pid;
    // }

}

/*
* get_term_pcb
*   DESCRIPTION: Returns PCB address of the process with given Terminal number.
*
*   INPUTS: int terminal_num - terminal number
*   OUTPUTS: pcb_t * - address to PCB of requested process
*   RETURN VALUE: pcb_t *
*/
pcb_t * get_term_pcb(int terminal_num) {
    int i;
    pcb_t * temp_pcb;

    /* Return -1 if given an invalid or inactive process ID */
    if (terminal_num > 2 || terminal_num < 0) { return (pcb_t *)-1; }

    for (i = 0; i < PROC_NUM; i++) {
        temp_pcb = (pcb_t *)(KSTACK_BOT - PCB_SIZE * i);
        if (temp_pcb == NULL) { continue; }
        if (temp_pcb->isTerm == 1 && temp_pcb->terminal == terminal_num) { 
            return temp_pcb; 
        }
    }

    return (pcb_t *)-1;
}


/*
* get_pcb
*   DESCRIPTION: Returns PCB of current process
*
*   INPUTS: none
*   OUTPUTS: pcb_t * - address to PCB of requested process
*   RETURN VALUE: pcb_t *
*/
pcb_t * get_curr_pcb() {
    if (curr_process > 6 || curr_process < 0) { return (pcb_t *)-1; }
    return (pcb_t *)(KSTACK_BOT - PCB_SIZE * curr_process);
}


/*
* get_pid
*   DESCRIPTION: Finds and returns a valid PID based on how many processes
*       are currently active. Returns -1 if max number of processes (six)
*       are already active.
*
*   INPUTS: none
*   OUTPUTS: int - PID (0-5)
*   RETURN VALUE: none
*/
int get_pid() {
    int i;

    /* Look thru process table for an inactive process (marked 0) and return it */
    for(i = 0; i < PROC_NUM; i++){
        if(proc_state[i] == 0){
            return i;
        }
    }

    /* Return -1 max number of processes allowed are already in use */
    return -1;
}


/*
* halt
*   DESCRIPTION: Halts a process
*
*   INPUTS: uint8_t status - 
*   OUTPUTS: 0 if successful,
*/
int32_t halt(uint8_t status){
    int32_t i;
    cli();
    pcb_t *cur_pcb = (pcb_t *)(KSTACK_BOT - PCB_SIZE * curr_process);
    pcb_t *par_pcb = (pcb_t *)(KSTACK_BOT - PCB_SIZE * cur_pcb->p_pid);

    //restore parent data 
    proc_state[cur_pcb->pid] = 0;

    //tss.esp0 = KSTACK_BOT - PCB_SIZE * curr_process - 4;      

    //close any relevant FDs 
    for(i = 0; i < FDESC_SIZE; i++){

        if(cur_pcb->file_array[i].flag){
            close(i);
        }
        cur_pcb->file_array[i].flag = 0;
    
    }

    curr_process = par_pcb->pid;

    if(cur_pcb->pid == cur_pcb->p_pid){
        execute((uint8_t *)"shell");
    }

    //restore parent paging (cr3)
    pid_page_map(par_pcb->pid);

    /* Update the tss.esp0 */
    tss.esp0 = cur_pcb->parent_esp;   

    sti();
    asm volatile(
        "movl   %0, %%esp   ;"
        "movl   %1, %%ebp   ;"
        "movl   %2, %%eax   ;"
        "jmp RETURN_FROM_IRET;"

        :
        :"r"(cur_pcb->parent_esp), "r"(cur_pcb->parent_ebp), "r"((uint32_t) status)  
        :"%eax"
    );


    return 0;

}

int32_t execute(const uint8_t * command){
     //pcb_t *pcb = (pcb_t *)(KSTACK_BOT - PCB_SIZE * curr_process);
    pcb_t * pcb = get_term_pcb(get_active_terminal());
    return execute_with_terminal_num(command, pcb->terminal, 0);
}


/*
* execute
*   DESCRIPTION: Executes a user level progam by and hands processor
*           off to the new program until it terminates.
*   NOTE: does not take args into account, need to work on this...
*
*   INPUTS: uint8_t * command - first string is filename followed by args to
*               be interpreted by getargs()
*   OUTPUTS: none
*   RETURN VALUE: 0 on success, -1 on failure
*	SIDE EFFECTS : Switches processor to user mode to run given user program
*/
int32_t execute_with_terminal_num(const uint8_t * command, int terminal_num, int isTerm){
    uint8_t inFile[CMD_LIMIT];  /* name of executable file           */
    uint32_t v_addr;            /* virtual addr of first instruction */
    dentry_t d;
    int pid;
    char errMsg[ERR_BUF] = "TOO MANY PROCESSES OPEN!";

    /* Ensure parameter is valid */
    if (command == NULL) { return -1; }

    cli();
    
    /* Ensure the given command is a valid executable file */   
    if (verify_file(command, inFile, &v_addr) == -1) { return -1; }
    
    /* Fetch a proccess id that is not in use */
    pid = get_pid();

    /* Ensure we don't launch too many processes */
    if (pid == -1) {
        terminal_write(errMsg, ERR_BUF);
        return 0; 
    }

    proc_state[pid] = 1;      /* Mark pid as used */
    
    /* Create PCB */
    pcb_init(pid, terminal_num, isTerm);
    pcb_t* pcb = (pcb_t *)(KSTACK_BOT - PCB_SIZE * pid);
    pcb_t* prev_pcb = get_term_pcb(terminal_num);

	/* Saving the current ESP and EBP into the PCB struct */
	asm volatile("			\n\
				movl %%ebp, %%eax 	\n\
				movl %%esp, %%ebx 	\n\
			"
			:"=a"(pcb->parent_ebp), "=b"(pcb->parent_esp)
            );

    /* Initialize paging for process */
    pid_page_map(pid);

    set_active_terminal(terminal_num);

    /* User-level Program Loader */
    read_dentry_by_name(inFile, &d);
    read_f(d.inode, (uint8_t *)PROGRAM_IMAGE_ADDR);

    /* Upate TSS ss0 and esp0 */
    tss.ss0 = KERNEL_DS;
    //tss.esp0 = (0x100000*8) - PCB_SIZE * pid - 4;
    tss.esp0 = KSTACK_BOT - (PCB_SIZE * pid) - MEM_FENCE;

    pcb->current_ebp = tss.esp0;
    pcb->current_esp = tss.esp0;


    /* update  prev_pcb's curent ebp esp*/
    if (prev_pcb != NULL) {
	asm volatile("			\n\
				movl %%ebp, %%eax 	\n\
				movl %%esp, %%ebx 	\n\
			"
			:"=a"(prev_pcb->current_ebp), "=b"(prev_pcb->current_ebp)
            );
    }
    
    sti();
    /* IRET setup and context switch */
    asm volatile(
            
            "mov $0x2B, %%ax;"
            "mov %%ax, %%ds;"
            "movl $0x83FFFFC, %%eax;"
            "pushl $0x2B;"
            "pushl %%eax;"
            "pushfl;"
            "pushl $0x23;"
            "pushl %0;"
            "iret;"
            "RETURN_FROM_IRET:;"
            "LEAVE;"
            "RET;"
            :	            /* no outputs         */
            :"r"(v_addr)	/* input              */
            :"%edx","%eax"	/* clobbered register */
            );

    return 0;
}


/*
* verify_file
*   DESCRIPTION: Simply verifies that the file command name is valid and
*       also checks that it is an executable file by checking for the 
*       ELF magic number.
*
*   INPUTS: uint8_t * command - first string is filename followed by args to
*               be interpreted by getargs()
*           uint8_t inFile[CMD_LIMIt] - buffer to hold the name of the file
*   OUTPUTS: 0 or -1
*   RETURN VALUE: 0 on success, -1 on failure
*	SIDE EFFECTS : places command filename into inFile buffer and starting
                   user address into v_addr
*/
int8_t verify_file(const uint8_t * cmd, uint8_t inFile[CMD_LIMIT], uint32_t * v_addr) {
    int i;  /* Loop index */

    /* Make sure passed in ptr is not a nullptr */
    if (cmd == NULL) { return -1; }

    /* Retrieve file name */
    memset(inFile, 0, CMD_LIMIT);
    for (i = 0; i < strlen((char *)cmd); i++) { 
        if (cmd[i] == ' ') break;
        inFile[i] = cmd[i];
    }
    inFile[i] = '\0';   /* Add sentinel to end of filename */

    /* Clear argsize and argBuf and parse arguments for getargs() */
    argSize -= argSize;
    memset(argBuf, 0, CMD_LIMIT);
    while (cmd[i] != '\0') {
        /* Skip spaces */
        if (cmd[i] == ' ') {
            i++;    continue;
        }

        /* Copy arg into argBuf */
        argBuf[argSize++] = cmd[i++];
    }

    /* Look in filesystem to see check if given file exists */
    dentry_t dentry_buf;
    if (read_dentry_by_name(inFile, &dentry_buf) == -1) { return -1; }

    /* Check to make sure file is an executable by checking for magic number */
    char fileBuf[BYTE_LEN]; /* Buffer to hold binary text */
    char magicBuf[BYTE_LEN] = {MAGIC_7F, MAGIC_E, MAGIC_L, MAGIC_F};
    read_f_by_name(inFile, 0, (uint8_t *)fileBuf, BYTE_LEN);
    if (strncmp(fileBuf, magicBuf, BYTE_LEN)) { return -1; }

    /* Retrieve address for first instruction from bytes 24-27 */
    read_f_by_name(inFile, START_ADDR, (uint8_t*)v_addr, BYTE_LEN);

    return 0;
}


/*
* read
*   DESCRIPTION: Reads a given file by creating a temporary PCB to retrieve the
*       starting offset and inode for that specific file and then jump to the
*       respective read handler for the file
*
*   INPUTS: uint32_t fd - file descriptor
*           void * buf - buffer to fill up with chars from file
*           int32_t nbytes - how many characters to read 
*   OUTPUTS: number of characters read
*   RETURN VALUE: int32_t - number of characters read 
*                 -1 on failure (no characters to read)
*/
int32_t read(int32_t fd, void * buf, int32_t nbytes){
    /* Ensure parameter is valid */
    if (buf == NULL) { return -1; }

    /* Check that we haven't exceeded max number of open files */
    if (fd >= FDESC_SIZE || fd < 0) { return -1; }

    /* Create a temporary PCB and initalize offest and inode for file to then jump to read() */
    pcb_t * temp_pcb = (pcb_t*)(KSTACK_BOT - (curr_process * PCB_SIZE));
    /////////////////////////////////
    if(temp_pcb->file_array[fd].flag == 0) return -1;
    /////////////////////////////////
    int offset = temp_pcb->file_array[fd].file_pos;
    int inode = temp_pcb->file_array[fd].inode;
    int count = temp_pcb->file_array[fd].read(inode,offset,buf,nbytes);

    /* Check if read handler returned an error, if so, return -1 */
    if (count < 0) return -1;
    

    /* Increment the read offset */
    temp_pcb->file_array[fd].file_pos += count;

    return count;
    
}


/*
* write
*   DESCRIPTION: Writes to a given file by creating a temporary PC to retrive
*       file inode and jump to the respective write handler for file 
*
*   INPUTS: uint32_t fd - file descriptor
*           void * buf - buffer to write to file
*           int32_t nbytes - how many characters were written
*   OUTPUTS: number of characters written
*   RETURN VALUE: int32_t - number of characters written
*                 -1 on failure (no characters to written)
*/
int32_t write(int32_t fd, const void * buf, int32_t nbytes){
    /* Ensure parameter is valid */
    if (buf == NULL) { return -1; }

    /* Check that we haven't exceeded max number of open files */
    if (fd >= FDESC_SIZE || fd < 0 || buf == NULL) return -1;

    /* Create a temporary PCB and initalize offest and inode for file to then jump to write() */
    pcb_t * temp_pcb = (pcb_t*)(KSTACK_BOT - (curr_process * PCB_SIZE));
     /////////////////////////////////
    if(temp_pcb->bitmap[fd] == 0) return -1;
    /////////////////////////////////
    int inode = temp_pcb->file_array[fd].inode;
    int count = temp_pcb->file_array[fd].write(inode, 0, (uint8_t *)buf, nbytes);

    /* Check if read handler returned an error, if so, return -1 */
    if (count < 0) return -1;

    return count;
}


/*
* open
*   DESCRIPTION: Opens a file and checks for what file type it is to
*       give it its proper initializations
*
*   INPUTS: const uint8_t * filename
*   OUTPUTS: file index
*   RETURN VALUE: file index or -1 on failure
*/
int32_t open(const uint8_t * filename){ 
    pcb_t * pcb;
    dentry_t temp_dentry;
    int i;

    /* Ensure parameter is valid */
    if (filename == NULL) { return -1; }

    /* initialize PCB to the bottom of kernel stack offest by 8MB * curr_process PID */
    pcb = (pcb_t *)(KSTACK_BOT - (PCB_SIZE * curr_process));

    /* If the filename does not exist, return -1 */
    if (read_dentry_by_name(filename, &temp_dentry) ==-1) return -1; 
    
    for (i = 2; i < FDESC_SIZE; i++) {
        if (pcb->bitmap[i] == 0) {
            pcb->bitmap[i] = 1;
            break;
        } 
    }
    if (i == FDESC_SIZE) return -1; //if no descriptors are free, return -1
    
    /* Check if the file type is an RTC file */
    if (temp_dentry.ftype == FILE_RTC) {
        pcb->file_array[i].read  = rtc_read_wrapper;
        pcb->file_array[i].write = rtc_write_wrapper;
        pcb->file_array[i].open  = rtc_open_wrapper;
        pcb->file_array[i].close = rtc_close_wrapper;
    }
    
    /* Check if file type is a directory file */
    if (temp_dentry.ftype == FILE_DIR) {
        open_dir();
        pcb->file_array[i].read  = read_dir_wrapper;
        pcb->file_array[i].write = write_dir_wrapper;
        pcb->file_array[i].open  = open_dir_wrapper;
        pcb->file_array[i].close = close_dir_wrapper;        
    }

    /* Check if file type is a regular file */
    if (temp_dentry.ftype == FILE_REG) {
        
        pcb->file_array[i].read  = read_f_wrapper;
        pcb->file_array[i].write = write_f_wrapper;
        pcb->file_array[i].open  = open_f_wrapper;
        pcb->file_array[i].close = close_f_wrapper;           
    }

    /* Set the inode for current file and set starting offset to 0 */
    pcb->file_array[i].inode = temp_dentry.inode;
    pcb->file_array[i].flag = 1; // file in use 
    pcb->file_array[i].file_pos = 0;
    
    return i;

}


/*
* close
*   DESCRIPTION: Closes a given file and updates the file array 
*
*   INPUTS: int32_t fd
*   OUTPUTS: 0 on success, -1 on fail
*/
int32_t close(int32_t fd){
    /* Make sure given file descriptor is within range */
/////////////////////////////////////////////////////////
    if (fd > 7 || fd < 2) return -1;///arg change to 7
/////////////////////////////////////////////////////////
    
    
    pcb_t *temp_pcb = (pcb_t *)(KSTACK_BOT - PCB_SIZE * curr_process);
    /////////////////////////////////
    if(temp_pcb->file_array[fd].flag == 0) return -1;
    /////////////////////////////////
    //int temp_pcb_addr = 0x800000-0x2000-curr_process*0x2000;

    temp_pcb->file_array[fd].flag = 0;
    temp_pcb->bitmap[fd] = 0;
    
    return 0;
}


/*
*  getargs
*   DESCRIPTION: Returns the last arguments passed into execute 
*
*   INPUTS: uint8_t * buf
*           int32_t nbytes
*   OUTPUTS: 0 on success, -1 on fail
*/
int32_t getargs(uint8_t * buf, int32_t nbytes) {
    int i;

    /* Make sure buffer constraints are fulfilled */
    if (buf == NULL || argSize == 0) { return -1; }

    /* Copy arguments into buf after resetting it */
    memset(buf, 0, strlen((char *)buf));
    for (i = 0; i < strlen((char *)argBuf); i++) { buf[i] = argBuf[i]; }
    buf[i] = '\0';

    return 0;
}


/*
*  vidmap
*   DESCRIPTION: 
*
*   INPUTS: uint8_t ** start_screen
*   OUTPUTS: 
*   RETURN VALUE: 
*/
int32_t vidmap(uint8_t ** start_screen) {   
    if(start_screen == NULL || start_screen == (uint8_t **)_4MB_){
        return -1;
    } 
    
    vidMem_page_map((int)(132 * _MB_));

    *start_screen = (uint8_t *)(132 * _MB_);

    return 132 * _MB_;
}




/*
*  set_handler
*   DESCRIPTION: 
*
*   INPUTS: uint32_t signum
*           void * handler_address
*   OUTPUTS: 
*   RETURN VALUE: 
*/
int32_t set_handler(int32_t signum, void * handler_address){
    return 0;
}


/*
*  sigreturn
*   DESCRIPTION: 
*
*   INPUTS: none
*   OUTPUTS: 
*   RETURN VALUE: 
*/
int32_t sigreturn(void){
    return 0;
}
